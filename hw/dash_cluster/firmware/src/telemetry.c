/*
 * telemetry.c
 *
 * The decode table, and the frame handler that walks it.
 *
 * Signals arrive big-endian, CAN's convention and what the DBC declares
 * (@0+/@0-). The sending firmware writes them that way deliberately - see the
 * byte-order note at the top of can_telemetry.c - so this reads them the same
 * way, by explicit byte access rather than by casting.
 */

#include <stddef.h>

#include "signal_store.h"
#include "telemetry.h"

typedef struct
{
	uint8_t FrameOffset;	/* TELEMETRY_OFFSET_* within the board's block */
	uint8_t ByteOffset;		/* where in the 8-byte payload */
	uint8_t Width;			/* 1 or 2 bytes */
	bool Signed;
	uint8_t Signal;			/* SignalId_t */
} TelemetrySignal_t;


/* Must match toyotune.dbc. Ordered by frame purely for readability; the
   handler filters on FrameOffset rather than relying on any ordering. */
static const TelemetrySignal_t TelemetrySignals[] =
{
	{ TELEMETRY_OFFSET_FAST, 0, 2, false, SIGNAL_RPM },
	{ TELEMETRY_OFFSET_FAST, 2, 2, false, SIGNAL_TPS_RAW },
	{ TELEMETRY_OFFSET_FAST, 4, 2, true,  SIGNAL_MAP },
	{ TELEMETRY_OFFSET_FAST, 6, 2, false, SIGNAL_INJ_PW },

	{ TELEMETRY_OFFSET_MEDIUM1, 0, 2, true,  SIGNAL_ECT },
	{ TELEMETRY_OFFSET_MEDIUM1, 2, 2, true,  SIGNAL_THA },
	{ TELEMETRY_OFFSET_MEDIUM1, 4, 2, true,  SIGNAL_THAM },
	{ TELEMETRY_OFFSET_MEDIUM1, 6, 2, false, SIGNAL_BATTERY },

	{ TELEMETRY_OFFSET_MEDIUM2, 0, 2, false, SIGNAL_INJ_DUTY },
	{ TELEMETRY_OFFSET_MEDIUM2, 2, 2, true,  SIGNAL_KNOCK_RETARD },
	{ TELEMETRY_OFFSET_MEDIUM2, 4, 1, false, SIGNAL_IGN_TIMING_RAW },
	{ TELEMETRY_OFFSET_MEDIUM2, 5, 1, false, SIGNAL_ISCV_DUTY_RAW },
	{ TELEMETRY_OFFSET_MEDIUM2, 6, 1, false, SIGNAL_LAMBDA_RAW },
	{ TELEMETRY_OFFSET_MEDIUM2, 7, 1, false, SIGNAL_PW_LOOP_MODE },

	{ TELEMETRY_OFFSET_MEDIUM3, 0, 2, true,  SIGNAL_KNOCK_CYL1 },
	{ TELEMETRY_OFFSET_MEDIUM3, 2, 2, true,  SIGNAL_KNOCK_CYL2 },
	{ TELEMETRY_OFFSET_MEDIUM3, 4, 2, true,  SIGNAL_KNOCK_CYL3 },
	{ TELEMETRY_OFFSET_MEDIUM3, 6, 1, false, SIGNAL_LAMBDA_TRIM_RAW },
	{ TELEMETRY_OFFSET_MEDIUM3, 7, 1, false, SIGNAL_MAX_RETARD_RAW },

	{ TELEMETRY_OFFSET_SLOW, 0, 1, false, SIGNAL_NV_TRIM_PIM_RAW },
	{ TELEMETRY_OFFSET_SLOW, 1, 1, false, SIGNAL_NV_TRIM_O2_RAW },
	{ TELEMETRY_OFFSET_SLOW, 2, 1, false, SIGNAL_FUEL_TRIM_RAW },
	{ TELEMETRY_OFFSET_SLOW, 3, 1, false, SIGNAL_ERROR_FLAGS1 },
	{ TELEMETRY_OFFSET_SLOW, 4, 1, false, SIGNAL_ERROR_FLAGS2 },
	{ TELEMETRY_OFFSET_SLOW, 5, 1, false, SIGNAL_FLAGS46 },
	{ TELEMETRY_OFFSET_SLOW, 6, 1, false, SIGNAL_FLAGS1 },
	{ TELEMETRY_OFFSET_SLOW, 7, 1, false, SIGNAL_LIMITER_FLAGS },

	{ TELEMETRY_OFFSET_INFO, 0, 1, false, SIGNAL_PROTOCOL_VERSION },
	{ TELEMETRY_OFFSET_INFO, 1, 1, false, SIGNAL_ECU_FAMILY },
	{ TELEMETRY_OFFSET_INFO, 2, 1, false, SIGNAL_CPU_INDEX },
	{ TELEMETRY_OFFSET_INFO, 4, 2, false, SIGNAL_TX_DROPPED },
	{ TELEMETRY_OFFSET_INFO, 6, 2, false, SIGNAL_BUS_OFF_RECOVERIES }
};

#define TELEMETRY_SIGNAL_COUNT \
	(sizeof(TelemetrySignals) / sizeof(TelemetrySignals[0]))

/* The RAW tier is deliberately not decoded here. It carries the whole DMA
   capture one slice at a time for reverse engineering, and a gauge has no use
   for it - reassembling 72 bytes every 550 ms would cost RAM and effort for
   something no page displays. */

static uint16_t TelemetryBase = TELEMETRY_BASE_CPU1;
static uint8_t SeenVersion;
static bool VersionMismatch;


/***************************************************************************************/
void Telemetry_Init(uint16_t Base)
{
	TelemetryBase = Base;
	SeenVersion = 0;
	VersionMismatch = false;
}


/***************************************************************************************/
static int32_t Telemetry_Read(const uint8_t *Data, uint8_t Offset,
                              uint8_t Width, bool IsSigned)
{
	if (Width == 2)
	{
		uint16_t Raw = (uint16_t)(((uint16_t)Data[Offset] << 8) | Data[Offset + 1]);

		return IsSigned ? (int32_t)(int16_t)Raw : (int32_t)Raw;
	}

	return IsSigned ? (int32_t)(int8_t)Data[Offset] : (int32_t)Data[Offset];
}


/***************************************************************************************/
bool Telemetry_Handle(uint16_t Id, const uint8_t *Data, uint8_t Length,
                      uint32_t NowMs)
{
	uint16_t Offset;
	uint32_t i;

	if (Data == NULL || Id < TelemetryBase)
		return false;

	Offset = (uint16_t)(Id - TelemetryBase);

	/* The diagnostic pair sits at +10/+11 in the same block, and the RAW tier
	   is not decoded, so anything outside the frames below is not ours. */
	if (Offset > TELEMETRY_OFFSET_INFO || Offset == TELEMETRY_OFFSET_RAW)
		return false;

	for (i = 0; i < TELEMETRY_SIGNAL_COUNT; i++)
	{
		const TelemetrySignal_t *Signal = &TelemetrySignals[i];

		if (Signal->FrameOffset != Offset)
			continue;

		/* A short frame is dropped rather than padded. Reading past the end
		   would decode whatever the driver left in the buffer, which looks
		   like a real reading and is not one. */
		if ((uint32_t)Signal->ByteOffset + Signal->Width > Length)
			continue;

		SignalStore_Set((SignalId_t)Signal->Signal,
		                Telemetry_Read(Data, Signal->ByteOffset,
		                               Signal->Width, Signal->Signed),
		                NowMs);
	}

	/* The version check is the point of the INFO frame: the sending firmware
	   and the DBC are edited separately and nothing enforces that they agree,
	   so a node that meets a layout it does not know should say so rather
	   than draw gauges from bytes that may have moved. */
	if (Offset == TELEMETRY_OFFSET_INFO && Length >= 1)
	{
		SeenVersion = Data[0];
		VersionMismatch = (SeenVersion != DASH_EXPECTED_PROTOCOL_VERSION);
	}

	return true;
}


/***************************************************************************************/
bool Telemetry_ProtocolMismatch(void)
{
	return VersionMismatch;
}


/***************************************************************************************/
uint8_t Telemetry_SeenProtocolVersion(void)
{
	return SeenVersion;
}
