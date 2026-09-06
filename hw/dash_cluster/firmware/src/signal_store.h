/*
 * signal_store.h
 *
 * Where decoded signals live, and how they cross between the two cores.
 *
 * Core 0 decodes CAN frames and writes here; core 1 renders and reads. That
 * is a single writer and a single reader, which is exactly the case a seqlock
 * suits: the writer never blocks, never allocates and never takes a lock, and
 * the reader retries on the rare occasion it catches a half-written value.
 * Blocking the writer would be the wrong trade here - the CAN interrupt is
 * the one thing on this node that genuinely cannot wait.
 *
 * STALENESS IS PART OF THE VALUE, NOT AN AFTERTHOUGHT.
 *
 * A gauge showing a frozen reading is worse than one that is obviously dead:
 * the driver cannot tell the difference between "coolant is 90" and "coolant
 * was 90 when the loom fell off". Every read therefore reports whether the
 * value is fresh, and the caller is expected to act on it.
 */

#ifndef SIGNAL_STORE_H_
#define SIGNAL_STORE_H_

#include <stdbool.h>
#include <stdint.h>

#include "signals.h"

/* A signal is stale once it is this many times its own period old. Three
   allows a dropped frame and its retry without flickering the display, while
   still catching a dead link inside half a second on the FAST tier. */
#define SIGNAL_STALE_PERIODS	(3)

/* Below this the node has heard nothing at all since boot. */
typedef struct
{
	int32_t Value;
	uint32_t UpdatedMs;
	bool Valid;			/* has ever been received */
	bool Fresh;			/* received recently enough to trust */
} SignalReading_t;


extern void SignalStore_Init(void);

/* Writer side - core 0 only. */
extern void SignalStore_Set(SignalId_t Id, int32_t Value, uint32_t NowMs);

/* Reader side - safe from the other core. */
extern SignalReading_t SignalStore_Get(SignalId_t Id, uint32_t NowMs);

/* True when nothing at all has arrived recently, whatever the signal - the
   difference between "the ECU is quiet" and "the bus is dead". */
extern bool SignalStore_LinkAlive(uint32_t NowMs);

/* Milliseconds since any frame was decoded, or UINT32_MAX if none ever was. */
extern uint32_t SignalStore_LinkAgeMs(uint32_t NowMs);

#endif /* SIGNAL_STORE_H_ */
