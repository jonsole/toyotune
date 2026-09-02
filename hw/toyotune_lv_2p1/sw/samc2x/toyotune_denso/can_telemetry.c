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
 */

#include <stddef.h>
#include <string.h>

#include "config.h"
#include "can_telemetry.h"
#include "can.h"
#include "ecu.h"
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


/* One signal copied out of a DMA block into a frame payload. Sizes are 1 or
   2 bytes; 2-byte signals are written big-endian, the automotive convention. */
typedef struct
{
	uint8_t Frame;			/* index into CanTelemetry_Frames */
	uint8_t Offset;			/* byte offset within that frame's payload */
	uint8_t Source;			/* CanTelemetry_Source_t */
	uint8_t Field;			/* byte offset within the DMA block */
	uint8_t Size;			/* 1 or 2 */
} CanTelemetry_Signal_t;


enum
{
	CAN_FRAME_FAST,
	CAN_FRAME_MEDIUM1,
	CAN_FRAME_MEDIUM2,
	CAN_FRAME_SLOW,
	CAN_FRAME_RAW,
	CAN_FRAME_COUNT
};


static const CanTelemetry_Frame_t CanTelemetry_Frames[CAN_FRAME_COUNT] =
{
	/* Engine-event signals: fast enough to log a throttle transient. */
	[CAN_FRAME_FAST]    = { TOYOTUNE_CAN_ID_FAST,    CAN_PERIOD_FAST,   8 },
	/* Thermal, electrical and ignition trim: seconds-scale at best. */
	[CAN_FRAME_MEDIUM1] = { TOYOTUNE_CAN_ID_MEDIUM1, CAN_PERIOD_MEDIUM, 8 },
	[CAN_FRAME_MEDIUM2] = { TOYOTUNE_CAN_ID_MEDIUM2, CAN_PERIOD_MEDIUM, 8 },
	/* Learned trims, flags and fault state: changes are rare and sticky. */
	[CAN_FRAME_SLOW]    = { TOYOTUNE_CAN_ID_SLOW,    CAN_PERIOD_SLOW,   8 },
	/* One slice of the raw dump per period - see CanTelemetry_SendRaw(). */
	[CAN_FRAME_RAW]     = { TOYOTUNE_CAN_ID_RAW,     CAN_PERIOD_RAW,    8 }
};


#if defined(TOYOTUNE_ECU_MR2) || defined(TOYOTUNE_ECU_ST205)

/* The MR2 and ST205 pairs share their DMA field slots, so one table serves
   both. A family that differs gets its own branch here, alongside its own
   structs in ecu.h. */
static const CanTelemetry_Signal_t CanTelemetry_Signals[] =
{
	/* Fast - what actually moves at engine-event rate. */
	{ CAN_FRAME_FAST, 0, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, RpmX5p12),   2 },
	{ CAN_FRAME_FAST, 2, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Tps),        2 },
	{ CAN_FRAME_FAST, 4, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Pim2),       2 },
	{ CAN_FRAME_FAST, 6, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, InjPwInj1),  2 },

	/* Medium 1 - temperatures, supply, mixture, idle. */
	{ CAN_FRAME_MEDIUM1, 0, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Ect),        2 },
	{ CAN_FRAME_MEDIUM1, 2, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Tha),        1 },
	{ CAN_FRAME_MEDIUM1, 3, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Tham),       1 },
	{ CAN_FRAME_MEDIUM1, 4, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Battery),    1 },
	{ CAN_FRAME_MEDIUM1, 5, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, AdcLambda),  1 },
	{ CAN_FRAME_MEDIUM1, 6, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, IgnTiming),  1 },
	{ CAN_FRAME_MEDIUM1, 7, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, IscvDuty),   1 },

	/* Medium 2 - knock, and the corrections CPU2 sends back. */
	{ CAN_FRAME_MEDIUM2, 0, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 0, 1 },
	{ CAN_FRAME_MEDIUM2, 1, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 1, 1 },
	{ CAN_FRAME_MEDIUM2, 2, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetardInfo) + 2, 1 },
	{ CAN_FRAME_MEDIUM2, 3, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, KnockRetard),      1 },
	{ CAN_FRAME_MEDIUM2, 4, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, KnockRetardCpu2),  1 },
	{ CAN_FRAME_MEDIUM2, 5, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, MaxRetard),        1 },
	{ CAN_FRAME_MEDIUM2, 6, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, LambdaTrim),       1 },
	{ CAN_FRAME_MEDIUM2, 7, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, PwLoopMode),       1 },

	/* Slow - learned trims, status and fault flags. */
	{ CAN_FRAME_SLOW, 0, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, NvTrimPim),   1 },
	{ CAN_FRAME_SLOW, 1, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, NvTrimO2),    1 },
	{ CAN_FRAME_SLOW, 2, CAN_SRC_CPU2_TO_CPU1, offsetof(ECU_DmaData2_t, FuelTrim),    1 },
	{ CAN_FRAME_SLOW, 3, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, ErrorFlags1), 1 },
	{ CAN_FRAME_SLOW, 4, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, ErrorFlags2), 1 },
	{ CAN_FRAME_SLOW, 5, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Flags46),     1 },
	{ CAN_FRAME_SLOW, 6, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, Flags1),      1 },
	{ CAN_FRAME_SLOW, 7, CAN_SRC_CPU1_TO_CPU2, offsetof(ECU_DmaData1_t, LimiterFlags), 1 }
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
	 && (CAN_PERIOD_RAW    % CAN_TELEMETRY_TICK_MS) == 0) ? 1 : -1];


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

		/* Fields are stored in the block in the ECU's byte order, which is
		   big-endian - the same order they go onto the wire - so the bytes
		   are copied straight across rather than being read as a number and
		   swapped back again. */
		Payload[Signal->Offset] = Block[Signal->Field];
		if (Signal->Size == 2)
			Payload[Signal->Offset + 1] = Block[Signal->Field + 1];
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
static void CanTelemetry_Task(void *Context)
{
	for (;;)
	{
		OS_SignalWait(CAN_TELEMETRY_SIGNAL_TICK);

		/* Before anything else, and regardless of whether there is data to
		   send: the controller does not leave bus-off without being told. */
		CAN_Poll();

		ECU_DmaData1_t Cpu1ToCpu2;
		ECU_DmaData2_t Cpu2ToCpu1;

		/* Nothing to report until both directions have produced a whole
		   frame; sending zeros would look like real readings. */
		if (!ECU_GetDmaSnapshot(&Cpu1ToCpu2, &Cpu2ToCpu1))
			continue;

		for (uint8_t i = 0; i < CAN_FRAME_COUNT; i++)
		{
			CanTelemetry.Elapsed[i] += CAN_TELEMETRY_TICK_MS;
			if (CanTelemetry.Elapsed[i] < CanTelemetry_Frames[i].PeriodMs)
				continue;

			CanTelemetry.Elapsed[i] = 0;

			if (i == CAN_FRAME_RAW)
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
