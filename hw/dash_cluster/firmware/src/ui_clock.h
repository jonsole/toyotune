/*
 * ui_clock.h
 *
 * Where a clock's hands point, and how its time reads. No hardware and no
 * display dependency, so it is host-tested; ui_clockpage.c draws it.
 *
 * Positions are 0..UI_POSITION_MAX round the whole face, as the gauges' are
 * round their sweep, so the same needle code draws the hands. The hour hand
 * moves with the minutes and the minute hand with the seconds, as a mechanical
 * clock's do - a clock whose hour hand jumps on the hour looks wrong in a way
 * that is hard to place.
 */
#ifndef UI_CLOCK_H_
#define UI_CLOCK_H_

#include <stdbool.h>
#include <stdint.h>

/* The fraction of a second, with this many fractional bits, so the second hand
   can sweep between the clock's own ticks rather than stepping once a second. */
#define UI_CLOCK_FRAC_BITS	(8)
#define UI_CLOCK_FRAC_ONE	(1u << UI_CLOCK_FRAC_BITS)

typedef struct
{
	uint16_t Hour;			/* 0..UI_POSITION_MAX round the face */
	uint16_t Minute;
	uint16_t Second;
} UiClockHands_t;

/* Hand positions for a time, FracQ being how far into the second it is.
   Hours beyond 11 wrap, as a 12-hour face does. */
extern UiClockHands_t UiClock_Hands(uint8_t Hours, uint8_t Minutes, uint8_t Seconds,
                                    uint32_t FracQ);

/* "21:47" into Out, or "--:--" when there is no trustworthy time. */
extern void UiClock_Format(bool Valid, uint8_t Hours, uint8_t Minutes,
                           char *Out, uint32_t Size);

#endif /* UI_CLOCK_H_ */
