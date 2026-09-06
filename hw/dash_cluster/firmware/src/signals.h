/*
 * signals.h
 *
 * Every value a gauge can display, and where it comes from on the bus.
 *
 * This is the dash node's half of the wire protocol the Toyotune boards
 * publish - see hw/toyotune_lv_2p1/sw/samc2x/toyotune_denso/toyotune.dbc and
 * can_telemetry.c, which are the authority. Protocol version 1.
 *
 * VALUES ARE STORED AS THE INTEGER THAT CAME OFF THE WIRE.
 *
 * The board already did the conversion to engineering units, so Ect arrives
 * as 8179 meaning 81.79 degC. Nothing here re-scales it: the descriptor
 * carries a decimal-place count instead, and formatting applies it at the
 * point of display. That keeps the whole node free of floating point, and
 * means a value can never be rounded twice on its way to a gauge.
 *
 * Adding a signal is a row in SignalDescriptors[] plus a row in the decode
 * table in telemetry.c. Neither should need new code.
 */

#ifndef SIGNALS_H_
#define SIGNALS_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum
{
	/* FAST, 20 ms - engine-event rate */
	SIGNAL_RPM,
	SIGNAL_TPS_RAW,
	SIGNAL_MAP,
	SIGNAL_INJ_PW,

	/* MEDIUM1, 100 ms - temperatures and supply */
	SIGNAL_ECT,
	SIGNAL_THA,
	SIGNAL_THAM,
	SIGNAL_BATTERY,

	/* MEDIUM2, 100 ms - fuelling and ignition trim */
	SIGNAL_INJ_DUTY,
	SIGNAL_KNOCK_RETARD,
	SIGNAL_IGN_TIMING_RAW,
	SIGNAL_ISCV_DUTY_RAW,
	SIGNAL_LAMBDA_RAW,
	SIGNAL_PW_LOOP_MODE,

	/* MEDIUM3, 100 ms - per-cylinder knock */
	SIGNAL_KNOCK_CYL1,
	SIGNAL_KNOCK_CYL2,
	SIGNAL_KNOCK_CYL3,
	SIGNAL_LAMBDA_TRIM_RAW,
	SIGNAL_MAX_RETARD_RAW,

	/* SLOW, 500 ms - learned trims and flags */
	SIGNAL_NV_TRIM_PIM_RAW,
	SIGNAL_NV_TRIM_O2_RAW,
	SIGNAL_FUEL_TRIM_RAW,
	SIGNAL_ERROR_FLAGS1,
	SIGNAL_ERROR_FLAGS2,
	SIGNAL_FLAGS46,
	SIGNAL_FLAGS1,
	SIGNAL_LIMITER_FLAGS,

	/* INFO, 1000 ms - the board's identity and health */
	SIGNAL_PROTOCOL_VERSION,
	SIGNAL_ECU_FAMILY,
	SIGNAL_CPU_INDEX,
	SIGNAL_TX_DROPPED,
	SIGNAL_BUS_OFF_RECOVERIES,

	SIGNAL_COUNT
} SignalId_t;


typedef struct
{
	const char *Name;		/* short label for a gauge */
	const char *Unit;		/* "rpm", "degC", "" for a raw count */
	uint8_t Decimals;		/* where the point goes: 8179 with 2 -> 81.79 */
	int32_t Min;			/* sane display range, not a hard limit */
	int32_t Max;
	uint16_t PeriodMs;		/* how often the board sends it */
} SignalDescriptor_t;


extern const SignalDescriptor_t SignalDescriptors[SIGNAL_COUNT];


/* The protocol version this build understands. The INFO frame carries the
   board's own version; a mismatch means the frame layout may have moved under
   us, and a gauge drawn from moved bytes is worse than no gauge. */
#define DASH_EXPECTED_PROTOCOL_VERSION	(1)

/* Format a value using its descriptor - "81.79", "3492", "-1.2".
   Returns Out. No floating point: the decimal point is inserted by hand. */
extern const char *Signal_Format(SignalId_t Id, int32_t Value,
                                 char *Out, uint32_t OutSize);

#endif /* SIGNALS_H_ */
