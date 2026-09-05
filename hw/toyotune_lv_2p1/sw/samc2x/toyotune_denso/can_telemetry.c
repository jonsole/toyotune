/*
 * can_telemetry.c
 *
 * Periodic CAN telemetry, built from the latest inter-CPU DMA capture.
 *
 * Why tiered rather than dumping the DMA blocks straight onto the bus: the
 * two blocks total 72 bytes and refresh every 4 ms, which is nine CAN frames
 * at 250 Hz - about 67% of a 500 kbit/s bus for one board, and more than the
 * whole bus once a second board joins. It is also mostly waste, because 4 ms
 * is far faster than the physics of most of these signals. Coolant
 * temperature and battery voltage do not move meaningfully between frames.
 *
 * So signals are grouped by how fast they actually change, and each group
 * goes out at its own rate. A full raw dump is still available, multiplexed
 * onto one identifier at a low rate, so nothing is lost for reverse
 * engineering - it just does not cost 250 Hz.
 *
 * Everything is table driven. Adding a signal is a row in
 * CanTelemetry_Signals; adding an ECU family whose DMA layout differs is a
 * new branch of that table, since the rows are offsets into the structs in
 * ecu.h and those are per-family.
 *
 * SIGNALS GO OUT IN ENGINEERING UNITS, NOT RAW COUNTS.
 *
 * Each row names a transform, and ecu_scale.c does the arithmetic - rpm,
 * microseconds, degrees C, kPa. The reason is not tidiness: a DBC can only
 * express raw * factor + offset, and the ECT/THA/THAM thermistors are not
 * linear in ADC counts, so no DBC could describe them at all. Once one signal
 * has to be converted here, converting the rest here too means every consumer
 * agrees by construction instead of by three copies of the same arithmetic.
 * Signals whose transfer function is not yet established keep CAN_XFORM_COPY*
 * and go out unscaled rather than being guessed at.
 *
 * Nothing is lost for reverse engineering: the RAW frame still carries every
 * byte of both DMA blocks untouched.
 *
 * BYTE ORDER IS NOW EXPLICIT, AND HAS TO BE.
 *
 * This module used to copy 16-bit fields straight across, which happened to
 * put them on the wire big-endian because the Denso CPU is big-endian and
 * that is also CAN's convention. That accident stops protecting anything the
 * moment a value is computed rather than copied: a scaled result is a SAMC21
 * number, in the SAMC21's little-endian order. Every field is therefore read
 * out of the block big-endian and written to the payload big-endian, by
 * explicit byte access - see CanTelemetry_ReadField/WriteField.
 */

#include <stddef.h>
#include <string.h>

#include "config.h"
#include "can_telemetry.h"
#include "can.h"
#include "ecu.h"
#include "ecu_scale.h"
#include "os.h"
#include "os_task_id.h"

#define CAN_TELEMETRY_SIGNAL_TICK	(OS_SIGNAL_USER << 0)

/* Tick granularity. Every frame period must be a multiple of this. */
#define CAN_TELEMETRY_TICK_MS		(10)

/* Transmit periods, defined once so the table below and the build-time check
   further down cannot drift apart. */
#define CAN_PERIOD_FAST				(20)
#define CAN_PERIOD_MEDIUM			(100)
#define CAN_PERIOD_SLOW				(500)
#define CAN_PERIOD_RAW				(50)
#define CAN_PERIOD_INFO				(1000)


/* Which DMA block a signal is sourced from. */
typedef enum
{
	CAN_SRC_CPU1_TO_CPU2,
	CAN_SRC_CPU2_TO_CPU1
} CanTelemetry_Source_t;


typedef struct
{
	uint16_t Id;
	uint16_t PeriodMs;
	uint8_t Length;
} CanTelemetry_Frame_t;


/* How a signal is converted on its way from the DMA block to the payload.
   The transform also fixes both widths, so a row cannot declare a source or
   destination size inconsistent with the conversion it names. */
typedef enum
{
	CAN_XFORM_COPY8,		/* 1 -> 1, unchanged: scaling not established yet */
	CAN_XFORM_COPY16,		/* 2 -> 2, unchanged */
	CAN_XFORM_RPM,			/* 2 -> 2, rpm */
	CAN_XFORM_INJPW,		/* 2 -> 2, microseconds */
	CAN_XFORM_PIM,			/* 2 -> 2, 0.1 kPa absolute, SIGNED */
	CAN_XFORM_ECT,			/* 2 -> 2, 0.01 degC, SIGNED (16-bit sensor) */
	CAN_XFORM_TEMP8,		/* 1 -> 2, 0.01 degC, SIGNED (8-bit sensor) */
	CAN_XFORM_BATTERY,		/* 1 -> 2, 0.01 V */
	CAN_XFORM_RETARD,		/* 1 -> 2, 0.01 degrees of retard, SIGNED */
	CAN_XFORM_COUNT
} CanTelemetry_Xform_t;


typedef struct
{
	uint8_t SrcSize;		/* bytes read from the DMA block */
	uint8_t DstSize;		/* bytes written to the payload */
} CanTelemetry_XformInfo_t;


static const CanTelemetry_XformInfo_t CanTelemetry_XformInfo[CAN_XFORM_COUNT] =
{
	[CAN_XFORM_COPY8]   = { 1, 1 },
	[CAN_XFORM_COPY16]  = { 2, 2 },
	[CAN_XFORM_RPM]     = { 2, 2 },
	[CAN_XFORM_INJPW]   = { 2, 2 },
	[CAN_XFORM_PIM]     = { 2, 2 },
	[CAN_XFORM_ECT]     = { 2, 2 },
	[CAN_XFORM_TEMP8]   = { 1, 2 },
	[CAN_XFORM_BATTERY] = { 1, 2 },
	[CAN_XFORM_RETARD]  = { 1, 2 }
};


/* One signal taken out of a DMA block, converted, and placed in a frame
   payload. Both sizes come from the transform, so they are not repeated
   here and cannot drift. */
typedef struct
{
	uint8_t Frame;			/* index into CanTelemetry_Frames */
	uint8_t Offset;			/* byte offset within that frame's payload */
	uint8_t Source;			/* CanTelemetry_Source_t */
	uint8_t Field;			/* byte offset within the DMA block */
	uint8_t Xform;			/* CanTelemetry_Xform_t */
} CanTelemetry_Signal_t;


enum
{
	CAN_FRAME_FAST,
	CAN_FRAME_MEDIUM1,
	CAN_FRAME_MEDIUM2,
	CAN_FRAME_MEDIUM3,
	CAN_FRAME_SLOW,
	CAN_FRAME_RAW,
	CAN_FRAME_INFO,
	CAN_FRAME_COUNT
};


/* Where the one derived signal lands. Injector duty is a function of two
   fields rather than a copy of one, so it cannot be a table row; it is
   filled in after the table loop. */
#define CAN_MEDIUM2_INJDUTY_OFFSET	(0)


static const CanTelemetry_Frame_t CanTelemetry_Frames[CAN_FRAME_COUNT] =
{
	/* Engine-event signals: fast enough to log a throttle transient. */
	[CAN_FRAME_FAST]    = { TOYOTUNE_CAN_ID_FAST,    CAN_PERIOD_FAST,   8 },
	/* Temperatures and supply. All four are scaled now, so each takes two
	   bytes and this frame holds exactly four signals. */
	[CAN_FRAME_MEDIUM1] = { TOYOTUNE_CAN_ID_MEDIUM1, CAN_PERIOD_MEDIUM, 8 },
	/* Fuelling and ignition trim. */
	[CAN_FRAME_MEDIUM2] = { TOYOTUNE_CAN_ID_MEDIUM2, CAN_PERIOD_MEDIUM, 8 },
	/* Per-cylinder knock. New: widening the retard signals to engineering
	   units pushed them out of MEDIUM2, which was already full at 8 bytes. */
	[CAN_FRAME_MEDIUM3] = { TOYOTUNE_CAN_ID_MEDIUM3, CAN_PERIOD_MEDIUM, 8 },
	/* Learned trims, flags and fault state: changes are rare and sticky. */
	[CAN_FRAME_SLOW]    = { TOYOTUNE_CAN_ID_SLOW,    CAN_PERIOD_SLOW,   8 },
	/* One slice of the raw dump per period - see CanTelemetry_SendRaw(). */
	[CAN_FRAME_RAW]     = { TOYOTUNE_CAN_ID_RAW,     CAN_PERIOD_RAW,    8 },
	/* Identity, protocol version and the CAN error counters - see
	   CanTelemetry_SendInfo(). Cheap insurance: it lets a consumer notice a
	   firmware/DBC mismatch and say so, rather than displaying plausible
	   nonsense, and it puts the drop counters on the bus where they are
	   actually visible. */
	[CAN_FRAME_INFO]    = { TOYOTUNE_CAN_ID_INFO,    CAN_PERIOD_INFO,   8 }
};


#if defined(TOYOTUNE_ECU_MR2) || defined(TOYOTUNE_ECU_ST205)

/* The MR2 and ST205 pairs share their DMA field slots, so one table serves
   both. A family that differs gets its own branch here, alongside its own
   structs in ecu.h. */
static const CanTelemetry_Signal_t CanTelemetry_Signals[] =
{
	/* Fast - what actually moves at engine-event rate.
	   Pim2 rather than Pim: Pim is the transient-compensated value fuel is
	   calculated from, whereas Pim2 is the measured manifold pressure and is
	   what the conversion in ecu_scale.c is calibrated against. A boost gauge
	   wants the measurement, not the fuelling model's view of it.
	   Tps stays raw - its endpoints are not established. */
	{ CAN_FRAME_FAST, 0, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, RpmX5p12),   CAN_XFORM_RPM },
	{ CAN_FRAME_FAST, 2, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Tps),        CAN_XFORM_COPY16 },
	{ CAN_FRAME_FAST, 4, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Pim2),       CAN_XFORM_PIM },
	{ CAN_FRAME_FAST, 6, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, InjPwInj1),  CAN_XFORM_INJPW },

	/* Medium 1 - temperatures and supply, all four scaled and so all 16-bit.
	   Note Ect is passed whole: its low byte carries two real bits of ADC
	   resolution, which the Q8 argument to ECU_ScaleTempC100 preserves. */
	{ CAN_FRAME_MEDIUM1, 0, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Ect),     CAN_XFORM_ECT },
	{ CAN_FRAME_MEDIUM1, 2, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Tha),     CAN_XFORM_TEMP8 },
	{ CAN_FRAME_MEDIUM1, 4, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Tham),    CAN_XFORM_TEMP8 },
	{ CAN_FRAME_MEDIUM1, 6, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Battery), CAN_XFORM_BATTERY },

	/* Medium 2 - fuelling and ignition trim. Bytes 0-1 are the derived
	   injector duty, filled in by CanTelemetry_SendFrame() rather than by a
	   row here. The four single-byte signals are still raw: none of their
	   transfer functions is established. */
	{ CAN_FRAME_MEDIUM2, 2, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetard), CAN_XFORM_RETARD },
	{ CAN_FRAME_MEDIUM2, 4, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, IgnTiming),   CAN_XFORM_COPY8 },
	{ CAN_FRAME_MEDIUM2, 5, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, IscvDuty),    CAN_XFORM_COPY8 },
	{ CAN_FRAME_MEDIUM2, 6, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, AdcLambda),   CAN_XFORM_COPY8 },
	{ CAN_FRAME_MEDIUM2, 7, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, PwLoopMode),  CAN_XFORM_COPY8 },

	/* Medium 3 - per-cylinder knock retard, scaled to degrees.
	   KnockRetardCpu2 has no seat here: it is CPU2's copy of a value already
	   carried as KnockRetard, and the RAW frame still has it. */
	{ CAN_FRAME_MEDIUM3, 0, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 0, CAN_XFORM_RETARD },
	{ CAN_FRAME_MEDIUM3, 2, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 1, CAN_XFORM_RETARD },
	{ CAN_FRAME_MEDIUM3, 4, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 2, CAN_XFORM_RETARD },
	{ CAN_FRAME_MEDIUM3, 6, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, LambdaTrim),          CAN_XFORM_COPY8 },
	{ CAN_FRAME_MEDIUM3, 7, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, MaxRetard),           CAN_XFORM_COPY8 },

	/* Slow - learned trims, status and fault flags. Flags are bit fields, so
	   they are copied rather than scaled; the DBC describes their bits. */
	{ CAN_FRAME_SLOW, 0, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, NvTrimPim),   CAN_XFORM_COPY8 },
	{ CAN_FRAME_SLOW, 1, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, NvTrimO2),    CAN_XFORM_COPY8 },
	{ CAN_FRAME_SLOW, 2, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, FuelTrim),    CAN_XFORM_COPY8 },
	{ CAN_FRAME_SLOW, 3, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, ErrorFlags1), CAN_XFORM_COPY8 },
	{ CAN_FRAME_SLOW, 4, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, ErrorFlags2), CAN_XFORM_COPY8 },
	{ CAN_FRAME_SLOW, 5, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Flags46),     CAN_XFORM_COPY8 },
	{ CAN_FRAME_SLOW, 6, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Flags1),      CAN_XFORM_COPY8 },
	{ CAN_FRAME_SLOW, 7, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, LimiterFlags), CAN_XFORM_COPY8 }
};

#else
#error "No CAN telemetry signal table for the selected ECU - add a branch here."
#endif

#define CAN_TELEMETRY_SIGNAL_COUNT \
	(sizeof(CanTelemetry_Signals) / sizeof(CanTelemetry_Signals[0]))

/* Every frame period must be a whole number of ticks, or a frame silently
   transmits at the wrong rate. Checked at build time. */
typedef char CanTelemetry_PeriodCheck[
	((CAN_PERIOD_FAST   % CAN_TELEMETRY_TICK_MS) == 0
	 && (CAN_PERIOD_MEDIUM % CAN_TELEMETRY_TICK_MS) == 0
	 && (CAN_PERIOD_SLOW   % CAN_TELEMETRY_TICK_MS) == 0
	 && (CAN_PERIOD_RAW    % CAN_TELEMETRY_TICK_MS) == 0
	 && (CAN_PERIOD_INFO   % CAN_TELEMETRY_TICK_MS) == 0) ? 1 : -1];

/* A row whose offset plus its transform's destination width runs past the
   payload would write over the next signal or off the end of the buffer, and
   this table is exactly the kind of thing that gets a row added in a hurry.
   C99 has no way to walk the table in a constant expression, and every Panic
   in this firmware currently compiles to nothing (see the #if 0 in debug.h),
   so a build-time or assert-based check would both be silent. The bound is
   therefore enforced where it costs one comparison and cannot be ignored:
   in CanTelemetry_SendFrame(), which skips an over-running signal rather
   than corrupting memory. */


/* The raw dump: the two DMA blocks back to back, sent 7 bytes at a time with
   a leading slice index, so a decoder can reassemble them. One slice per
   CAN_FRAME_RAW period, cycling. */
#define CAN_RAW_TOTAL_BYTES \
	(TOYOTUNE_DMA_CPU1_TO_CPU2_SIZE + TOYOTUNE_DMA_CPU2_TO_CPU1_SIZE)
#define CAN_RAW_PAYLOAD		(7)
#define CAN_RAW_SLICES \
	((CAN_RAW_TOTAL_BYTES + CAN_RAW_PAYLOAD - 1) / CAN_RAW_PAYLOAD)


static struct
{
	uint16_t Elapsed[CAN_FRAME_COUNT];	/* ms since each frame last went out */
	uint8_t RawSlice;
	uint32_t Stack[256];
} CanTelemetry;

static volatile uint16_t CanTelemetry_TickMs;


/***************************************************************************************/
void CanTelemetry_TimerTick(void)
{
	CanTelemetry_TickMs += 1;
	if (CanTelemetry_TickMs >= CAN_TELEMETRY_TICK_MS)
	{
		CanTelemetry_TickMs = 0;
		OS_SignalSend(CAN_TELEMETRY_TASK_ID, CAN_TELEMETRY_SIGNAL_TICK);
	}
}


/***************************************************************************************/
/* Read a field out of a DMA block. The Denso CPU is big-endian, so a 16-bit
   field is high byte first. Done by byte access rather than by casting to a
   uint16_t: it needs no alignment assumption about the block, and it states
   the byte order at the point it matters instead of relying on ECU_Be16()
   being remembered at every call site. */
static uint32_t CanTelemetry_ReadField(const uint8_t *Block, uint8_t Field, uint8_t Size)
{
	if (Size == 2)
		return ((uint32_t)Block[Field] << 8) | (uint32_t)Block[Field + 1];

	return (uint32_t)Block[Field];
}


/* Write a value into the payload big-endian, CAN's convention and the order
   the DBC declares (@0+). This is the step that used to be free, back when
   fields were copied byte for byte out of a big-endian block; a computed
   value is a SAMC21 number and has to be placed deliberately. */
static void CanTelemetry_WriteField(uint8_t *Payload, uint8_t Offset, uint8_t Size,
                                    uint32_t Value)
{
	if (Size == 2)
	{
		Payload[Offset]     = (uint8_t)(Value >> 8);
		Payload[Offset + 1] = (uint8_t)(Value & 0xFFu);
	}
	else
	{
		Payload[Offset] = (uint8_t)(Value & 0xFFu);
	}
}


/* Apply a transform. Signed results are cast to their unsigned counterpart of
   the same width before being written, so the two's-complement bits land on
   the wire unchanged; the DBC declares those signals signed and the decoder
   puts the sign back. */
static uint32_t CanTelemetry_Apply(uint8_t Xform, uint32_t Raw)
{
	switch ((CanTelemetry_Xform_t)Xform)
	{
	case CAN_XFORM_RPM:
		return ECU_ScaleRpm((uint16_t)Raw);

	case CAN_XFORM_INJPW:
		return ECU_ScaleInjPwUs((uint16_t)Raw);

	case CAN_XFORM_PIM:
		return (uint16_t)ECU_ScalePimKpa10((uint16_t)Raw);

	case CAN_XFORM_ECT:
		/* Already Q8: the 16-bit field is the ECU value times 256. */
		return (uint16_t)ECU_ScaleTempC100((uint16_t)Raw);

	case CAN_XFORM_TEMP8:
		/* 8-bit sensor, so shift it into the same Q8 the curve expects. */
		return (uint16_t)ECU_ScaleTempC100((uint16_t)(Raw << 8));

	case CAN_XFORM_BATTERY:
		return ECU_ScaleBatteryV100((uint8_t)Raw);

	case CAN_XFORM_RETARD:
		return (uint16_t)ECU_ScaleRetardDeg100((uint8_t)Raw);

	case CAN_XFORM_COPY8:
	case CAN_XFORM_COPY16:
	case CAN_XFORM_COUNT:
	default:
		return Raw;
	}
}


/***************************************************************************************/
static void CanTelemetry_SendFrame(uint8_t FrameIndex,
                                   const ECU_DmaData1_t *Cpu1ToCpu2,
                                   const ECU_DmaData2_t *Cpu2ToCpu1)
{
	const CanTelemetry_Frame_t *Frame = &CanTelemetry_Frames[FrameIndex];
	uint8_t Payload[8];

	memset(Payload, 0, sizeof(Payload));

	for (uint32_t i = 0; i < CAN_TELEMETRY_SIGNAL_COUNT; i++)
	{
		const CanTelemetry_Signal_t *Signal = &CanTelemetry_Signals[i];
		if (Signal->Frame != FrameIndex)
			continue;

		const uint8_t *Block = (Signal->Source == CAN_SRC_CPU1_TO_CPU2)
		                     ? (const uint8_t *)Cpu1ToCpu2
		                     : (const uint8_t *)Cpu2ToCpu1;
		const CanTelemetry_XformInfo_t *Info = &CanTelemetry_XformInfo[Signal->Xform];

		/* See the note by CanTelemetry_PeriodCheck: drop a row that would
		   run past the payload rather than writing over its neighbour. */
		if ((uint32_t)Signal->Offset + Info->DstSize > Frame->Length)
			continue;

		uint32_t Raw = CanTelemetry_ReadField(Block, Signal->Field, Info->SrcSize);
		uint32_t Value = CanTelemetry_Apply(Signal->Xform, Raw);

		CanTelemetry_WriteField(Payload, Signal->Offset, Info->DstSize, Value);
	}

	/* Derived signals. Injector duty is a function of two fields rather than
	   a copy of one, so it has no row in the table; it goes here instead of
	   contorting the table's shape for a single case.

	   It costs no bus space - both its inputs already travel in FAST - and it
	   is the number a driver actually wants, since a pulse width means little
	   without the engine speed to divide it by. */
	if (FrameIndex == CAN_FRAME_MEDIUM2)
	{
		uint16_t Rpm = ECU_ScaleRpm((uint16_t)CanTelemetry_ReadField(
			(const uint8_t *)Cpu2ToCpu1, offsetof(ECU_DmaData2_t, RpmX5p12), 2));
		uint16_t PwUs = ECU_ScaleInjPwUs((uint16_t)CanTelemetry_ReadField(
			(const uint8_t *)Cpu1ToCpu2, offsetof(ECU_DmaData1_t, InjPwInj1), 2));

		CanTelemetry_WriteField(Payload, CAN_MEDIUM2_INJDUTY_OFFSET, 2,
		                        ECU_ScaleInjDutyPct100(PwUs, Rpm));
	}

	CAN_TxStandard(Frame->Id, Payload, Frame->Length);
}


/***************************************************************************************/
static void CanTelemetry_SendRaw(const ECU_DmaData1_t *Cpu1ToCpu2,
                                 const ECU_DmaData2_t *Cpu2ToCpu1)
{
	uint8_t Payload[8];
	uint8_t Slice = CanTelemetry.RawSlice;
	uint32_t Start = (uint32_t)Slice * CAN_RAW_PAYLOAD;
	uint32_t Count = CAN_RAW_TOTAL_BYTES - Start;

	if (Count > CAN_RAW_PAYLOAD)
		Count = CAN_RAW_PAYLOAD;

	memset(Payload, 0, sizeof(Payload));
	Payload[0] = Slice;

	for (uint32_t i = 0; i < Count; i++)
	{
		uint32_t Index = Start + i;
		Payload[1 + i] = (Index < TOYOTUNE_DMA_CPU1_TO_CPU2_SIZE)
		               ? ((const uint8_t *)Cpu1ToCpu2)[Index]
		               : ((const uint8_t *)Cpu2ToCpu1)[Index - TOYOTUNE_DMA_CPU1_TO_CPU2_SIZE];
	}

	CAN_TxStandard(CanTelemetry_Frames[CAN_FRAME_RAW].Id, Payload, 1 + Count);

	CanTelemetry.RawSlice = (uint8_t)((Slice + 1) % CAN_RAW_SLICES);
}


/***************************************************************************************/
/* Identity, protocol version and the CAN error counters.
 *
 * The version byte is the point of this frame. Nothing enforces that the
 * firmware's frame layout and toyotune.dbc agree - they are edited in
 * different places by different hands - so a consumer that finds a version it
 * does not know can say "I cannot decode this build" instead of drawing a
 * gauge from bytes that have moved.
 *
 * The counters are here because CAN_TxDropped and CAN_BusOffRecoveries are
 * the first things worth reading when telemetry goes quiet, and until now
 * they were reachable only over SWD - which is exactly the debugger you do
 * not have attached when the car is on the road. */
static void CanTelemetry_SendInfo(void)
{
	uint8_t Payload[8];

	memset(Payload, 0, sizeof(Payload));

	Payload[0] = TOYOTUNE_TELEMETRY_PROTOCOL_VERSION;

#if defined(TOYOTUNE_ECU_MR2)
	Payload[1] = 0;
#elif defined(TOYOTUNE_ECU_ST205)
	Payload[1] = 1;
#endif

#if defined(TOYOTUNE_CPU1)
	Payload[2] = 1;
#else
	Payload[2] = 2;
#endif

	/* Payload[3] reserved - zero, so a later use is distinguishable. */

	/* Saturating: these only ever matter as "zero" versus "not zero, and
	   getting worse", and a wrapped counter would read as recovery. */
	CanTelemetry_WriteField(Payload, 4, 2,
		(CAN_TxDropped > 0xFFFFu) ? 0xFFFFu : CAN_TxDropped);
	CanTelemetry_WriteField(Payload, 6, 2,
		(CAN_BusOffRecoveries > 0xFFFFu) ? 0xFFFFu : CAN_BusOffRecoveries);

	CAN_TxStandard(CanTelemetry_Frames[CAN_FRAME_INFO].Id, Payload,
	               CanTelemetry_Frames[CAN_FRAME_INFO].Length);
}


/***************************************************************************************/
static void CanTelemetry_Task(void *Context)
{
	(void)Context;

	for (;;)
	{
		OS_SignalWait(CAN_TELEMETRY_SIGNAL_TICK);

		/* Before anything else, and regardless of whether there is data to
		   send: the controller does not leave bus-off without being told. */
		CAN_Poll();

		ECU_DmaData1_t Cpu1ToCpu2;
		ECU_DmaData2_t Cpu2ToCpu1;

		/* Nothing to report from the ECU until both directions have produced
		   a whole frame; sending zeros would look like real readings.
		   INFO is the exception, handled below: a board with no ECU data is
		   precisely when someone wants to see that it is alive and what its
		   error counters say. */
		bool HaveData = ECU_GetDmaSnapshot(&Cpu1ToCpu2, &Cpu2ToCpu1);

		for (uint8_t i = 0; i < CAN_FRAME_COUNT; i++)
		{
			CanTelemetry.Elapsed[i] += CAN_TELEMETRY_TICK_MS;
			if (CanTelemetry.Elapsed[i] < CanTelemetry_Frames[i].PeriodMs)
				continue;

			CanTelemetry.Elapsed[i] = 0;

			if (i == CAN_FRAME_INFO)
				CanTelemetry_SendInfo();
			else if (!HaveData)
				continue;
			else if (i == CAN_FRAME_RAW)
				CanTelemetry_SendRaw(&Cpu1ToCpu2, &Cpu2ToCpu1);
			else
				CanTelemetry_SendFrame(i, &Cpu1ToCpu2, &Cpu2ToCpu1);
		}
	}
}


/***************************************************************************************/
void CanTelemetry_Init(void)
{
	memset(&CanTelemetry, 0, sizeof(CanTelemetry));

	/* Stagger the tiers so their periods do not all expire on the same tick,
	   which would burst several frames back to back. */
	for (uint8_t i = 0; i < CAN_FRAME_COUNT; i++)
		CanTelemetry.Elapsed[i] = (uint16_t)(i * CAN_TELEMETRY_TICK_MS);

	OS_TaskInit(CAN_TELEMETRY_TASK_ID, CanTelemetry_Task, &CanTelemetry,
	            CanTelemetry.Stack, sizeof(CanTelemetry.Stack));
}
