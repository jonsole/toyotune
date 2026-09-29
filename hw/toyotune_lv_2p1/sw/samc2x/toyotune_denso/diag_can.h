/*
 * diag_can.h
 *
 * CAN command interface to the Denso diagnostic link: one command frame in,
 * one response frame out. See diag_can.c for the frame layout.
 */

#ifndef DIAG_CAN_H_
#define DIAG_CAN_H_

#include <stdbool.h>
#include <stdint.h>

void DiagCan_Init(void);


/* Values the board reads out of the ECU's RAM on a standing schedule, for
   telemetry - as opposed to reads a host asks for over CAN. Updated by the
   diag task, read by the telemetry task, so every field is volatile and each
   is a single byte or a counter that is only ever compared for change.

   Reads counts completed reads: telemetry uses it to tell a live value from
   one that stopped updating, and sends nothing rather than repeat a stale
   byte - a frozen, plausible reading is the worst way for a gauge to fail. */
typedef struct
{
	volatile uint8_t KnockRetard;	/* CPU1 var_knock_retard, ~0.5 deg per count */
	volatile uint32_t Reads;
} DiagCan_Live_t;

extern DiagCan_Live_t DiagCan_Live;

/* A plain copy for telemetry to read fields out of by offsetof(), the same way
   it reads the DMA blocks - one snapshot per frame, so no field is read from
   the shared block twice. */
typedef struct
{
	uint8_t KnockRetard;
} DiagCan_LiveBlock_t;

/* Whether this build reads anything live at all. The addresses are one ROM's
   RAM layout and one CPU's, so a board attached to anything else must not
   read them: the same address on CPU2 is a different variable entirely. */
extern bool DiagCan_LiveEnabled(void);

#endif /* DIAG_CAN_H_ */
