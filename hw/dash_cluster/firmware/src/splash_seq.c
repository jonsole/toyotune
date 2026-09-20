/*
 * splash_seq.c - the power-on splash's timeline. See splash_seq.h.
 */

#include "splash_seq.h"


/***************************************************************************************/
/* Brightness along a fade, 0 at the start to SPLASH_FULL at the end.

   Squared, not linear: the eye judges a dark screen by ratio, so a straight
   ramp spends its first half looking suddenly bright and its second half
   barely changing. Rising along t^2 looks like an even fade. */
static uint32_t Splash_Ramp(uint32_t Into, uint32_t Length)
{
	uint64_t T;

	if (Length == 0u || Into >= Length)
		return SPLASH_FULL;

	T = ((uint64_t)Into * SPLASH_FULL) / Length;		/* 0..SPLASH_FULL, linear */
	return (uint32_t)((T * T) / SPLASH_FULL);
}


/***************************************************************************************/
/* One image's life: in, hold, out. Level for Into ms into it, or false once
   it is over. */
static bool Splash_Phase(uint32_t Into, uint32_t In, uint32_t Hold, uint32_t Out,
                         uint32_t *Level)
{
	if (Into < In)
	{
		*Level = Splash_Ramp(Into, In);
		return true;
	}
	Into -= In;
	if (Into < Hold)
	{
		*Level = SPLASH_FULL;
		return true;
	}
	Into -= Hold;
	if (Into < Out)
	{
		*Level = SPLASH_FULL - Splash_Ramp(Into, Out);
		return true;
	}
	return false;
}


/***************************************************************************************/
uint32_t Splash_TotalMs(bool HasLetter)
{
	uint32_t Ms = SPLASH_EMBLEM_IN_MS + SPLASH_EMBLEM_HOLD_MS + SPLASH_EMBLEM_OUT_MS;

	if (HasLetter)
		Ms += SPLASH_LETTER_IN_MS + SPLASH_LETTER_HOLD_MS + SPLASH_LETTER_OUT_MS;
	return Ms;
}


/***************************************************************************************/
SplashFrame_t Splash_At(uint32_t ElapsedMs, bool HasLetter)
{
	SplashFrame_t F;
	uint32_t Emblem = SPLASH_EMBLEM_IN_MS + SPLASH_EMBLEM_HOLD_MS + SPLASH_EMBLEM_OUT_MS;

	F.Level = 0u;

	if (Splash_Phase(ElapsedMs, SPLASH_EMBLEM_IN_MS, SPLASH_EMBLEM_HOLD_MS,
	                 SPLASH_EMBLEM_OUT_MS, &F.Level))
	{
		F.Show = SPLASH_SHOW_EMBLEM;
		return F;
	}

	if (HasLetter && Splash_Phase(ElapsedMs - Emblem, SPLASH_LETTER_IN_MS,
	                              SPLASH_LETTER_HOLD_MS, SPLASH_LETTER_OUT_MS, &F.Level))
	{
		F.Show = SPLASH_SHOW_LETTER;
		return F;
	}

	F.Show = SPLASH_SHOW_DONE;
	F.Level = 0u;
	return F;
}
