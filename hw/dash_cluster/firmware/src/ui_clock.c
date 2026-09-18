/*
 * ui_clock.c - the clock's hands. See ui_clock.h.
 */

#include "ui_clock.h"

#include <stdio.h>

#include "ui_model.h"


/***************************************************************************************/
UiClockHands_t UiClock_Hands(uint8_t Hours, uint8_t Minutes, uint8_t Seconds,
                             uint32_t FracQ)
{
	UiClockHands_t H;
	uint32_t Ticks;			/* fractions of a second since midnight, 12-hour */

	if (FracQ >= UI_CLOCK_FRAC_ONE)
		FracQ = UI_CLOCK_FRAC_ONE - 1u;

	Ticks = ((uint32_t)(Hours % 12u) * 3600u) + ((uint32_t)Minutes * 60u)
	        + (uint32_t)Seconds;
	Ticks = (Ticks * UI_CLOCK_FRAC_ONE) + FracQ;

	/* Twelve hours, an hour and a minute, each as a fraction of the face.
	   In 64 bits: a whole twelve hours in fractions of a second is 11 million,
	   and multiplying that by the face's 1000 steps overflows 32 - which it
	   did, putting the hour hand at twenty past two at half past six. */
	H.Hour = (uint16_t)(((uint64_t)Ticks * UI_POSITION_MAX)
	                    / (43200u * UI_CLOCK_FRAC_ONE));
	H.Minute = (uint16_t)(((uint64_t)(Ticks % (3600u * UI_CLOCK_FRAC_ONE))
	                       * UI_POSITION_MAX)
	                      / (3600u * UI_CLOCK_FRAC_ONE));
	H.Second = (uint16_t)(((uint64_t)(Ticks % (60u * UI_CLOCK_FRAC_ONE))
	                       * UI_POSITION_MAX)
	                      / (60u * UI_CLOCK_FRAC_ONE));
	return H;
}


/***************************************************************************************/
void UiClock_Format(bool Valid, uint8_t Hours, uint8_t Minutes, char *Out, uint32_t Size)
{
	if (!Valid || Hours > 23u || Minutes > 59u)
	{
		/* Never a plausible wrong time: a dash clock reading 00:00 would be
		   taken for midnight rather than for "nobody has told me". */
		(void)snprintf(Out, Size, "--:--");
		return;
	}

	(void)snprintf(Out, Size, "%02u:%02u", (unsigned)Hours, (unsigned)Minutes);
}
