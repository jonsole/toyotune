/*
 * signal_store.c
 *
 * Seqlock-protected signal storage, single writer, single reader.
 *
 * The sequence counter is odd while a write is in progress and even when it
 * is settled. A reader samples it, copies the value, and samples again: if
 * either sample was odd, or the two differ, the writer was inside the value
 * and the read is retried. The writer is never delayed by a reader.
 *
 * The barriers matter and are not decoration. Without them the compiler or
 * the store buffer is free to reorder the value write past the counter
 * increment, which would let a reader see a settled counter alongside a
 * half-updated value - the exact race the seqlock exists to prevent. On a
 * single-core build the barriers cost nothing measurable and keep the code
 * identical, which is worth more than the cycles.
 */

#include <string.h>

#include "signal_store.h"

#if defined(PICO_ON_DEVICE) || defined(__ARM_ARCH)
#include "hardware/sync.h"
/* On the target this must be a real data memory barrier: the two cores are
   genuinely concurrent, and a store buffer reordering the value past the
   sequence counter is exactly the race the seqlock exists to prevent. */
#define STORE_BARRIER()		__dmb()
#elif defined(_MSC_VER)
#include <intrin.h>
#define STORE_BARRIER()		_ReadWriteBarrier()
#else
/* Host builds for the unit tests are single-threaded, so stopping the
   compiler reordering is enough - there is no second core to race with. */
#define STORE_BARRIER()		__asm__ __volatile__("" ::: "memory")
#endif

typedef struct
{
	volatile uint32_t Seq;
	int32_t Value;
	uint32_t UpdatedMs;
	bool Valid;
} SignalSlot_t;

static SignalSlot_t Slots[SIGNAL_COUNT];
static volatile uint32_t LastFrameMs;
static volatile bool AnyFrameSeen;


/***************************************************************************************/
void SignalStore_Init(void)
{
	memset(Slots, 0, sizeof(Slots));
	LastFrameMs = 0;
	AnyFrameSeen = false;
}


/***************************************************************************************/
void SignalStore_Set(SignalId_t Id, int32_t Value, uint32_t NowMs)
{
	SignalSlot_t *Slot;

	if (Id >= SIGNAL_COUNT)
		return;

	Slot = &Slots[Id];

	Slot->Seq++;			/* now odd - a write is in progress */
	STORE_BARRIER();

	Slot->Value = Value;
	Slot->UpdatedMs = NowMs;
	Slot->Valid = true;

	STORE_BARRIER();
	Slot->Seq++;			/* even again - settled */

	LastFrameMs = NowMs;
	AnyFrameSeen = true;
}


/***************************************************************************************/
SignalReading_t SignalStore_Get(SignalId_t Id, uint32_t NowMs)
{
	SignalReading_t Out = { 0, 0, false, false };
	const SignalSlot_t *Slot;
	uint32_t Before, After;
	uint32_t AgeMs, StaleMs;

	if (Id >= SIGNAL_COUNT)
		return Out;

	Slot = &Slots[Id];

	/* Retry until the writer is not inside the value. Unbounded in principle,
	   but the writer holds it for a handful of instructions, so in practice
	   this spins at most once. */
	do
	{
		Before = Slot->Seq;
		STORE_BARRIER();

		Out.Value = Slot->Value;
		Out.UpdatedMs = Slot->UpdatedMs;
		Out.Valid = Slot->Valid;

		STORE_BARRIER();
		After = Slot->Seq;
	} while ((Before & 1u) || (Before != After));

	if (!Out.Valid)
		return Out;

	/* Unsigned subtraction, so this stays correct across the 49-day wrap of a
	   millisecond counter rather than reporting a huge age once. */
	AgeMs = NowMs - Out.UpdatedMs;
	StaleMs = (uint32_t)SignalDescriptors[Id].PeriodMs * SIGNAL_STALE_PERIODS;
	Out.Fresh = (AgeMs <= StaleMs);

	return Out;
}


/***************************************************************************************/
uint32_t SignalStore_LinkAgeMs(uint32_t NowMs)
{
	if (!AnyFrameSeen)
		return UINT32_MAX;

	return NowMs - LastFrameMs;
}


/***************************************************************************************/
/* The fastest tier is 20 ms, so a bus that has said nothing for half a second
   has stopped rather than merely paused. Deliberately generous: this drives a
   "no link" takeover on the display, and flickering that on a momentary gap
   would be worse than reacting a little late. */
#define LINK_DEAD_MS	(500)

bool SignalStore_LinkAlive(uint32_t NowMs)
{
	return SignalStore_LinkAgeMs(NowMs) <= LINK_DEAD_MS;
}
