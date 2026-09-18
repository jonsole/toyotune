/*
 * ui_clockpage.c - the clock page. See ui_clockpage.h.
 */

#include "ui_clockpage.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "clock_link.h"
#include "dash_font.h"
#include "ui_clock.h"
#include "ui_draw.h"
#include "ui_gauge.h"
#include "ui_model.h"
#include "ui_text.h"

extern const DashFont_t dash_font_clock_36;
extern const DashFont_t dash_font_clock_160;

/* The time as the node believes it, and where it came from. */
static RtcTime_t	Now;
static bool		Valid;
static uint32_t		SecondStartUs;		/* when the current second began */
static uint32_t		NextReadMs;
static uint32_t		Adopted;		/* times taken from the bus */


/***************************************************************************************/
void UiClockPage_Init(void)
{
	memset(&Now, 0, sizeof(Now));
	Valid = false;
	SecondStartUs = time_us_32();
	NextReadMs = 0u;
}


/***************************************************************************************/
bool UiClockPage_Time(RtcTime_t *Out)
{
	*Out = Now;
	return Valid;
}


/***************************************************************************************/
static void UiClockPage_Tick(void)
{
	Now.Seconds++;
	if (Now.Seconds >= 60u)
	{
		Now.Seconds = 0u;
		Now.Minutes++;
		if (Now.Minutes >= 60u)
		{
			Now.Minutes = 0u;
			Now.Hours = (uint8_t)((Now.Hours + 1u) % 24u);
		}
	}
}


/***************************************************************************************/
void UiClockPage_Sample(uint32_t NowMs, uint32_t FrameUs)
{
	RtcTime_t Announced;
	uint32_t Us = time_us_32();

	(void)FrameUs;

	/* Anything the bus announced is authoritative: it comes from a device with
	   a reason to know, and this node's clock is not backed up. */
	if (ClockLink_Take(&Announced))
	{
		if (Rtc_Write(&Announced))
		{
			Now = Announced;
			Valid = true;
			SecondStartUs = Us;
			Adopted++;
		}
	}

	/* Advance between RTC reads, so the second hand sweeps. */
	while (Valid && (uint32_t)(Us - SecondStartUs) >= 1000000u)
	{
		SecondStartUs += 1000000u;
		UiClockPage_Tick();
	}

	if ((int32_t)(NowMs - NextReadMs) >= 0)
	{
		RtcTime_t T;

		NextReadMs = NowMs + UI_CLOCKPAGE_READ_MS;
		if (Rtc_Read(&T))
		{
			/* Take the clock's own second boundary when it moves on, so the
			   sweep stays in step with it rather than with the frame clock. */
			if (!Valid || T.Seconds != Now.Seconds || T.Minutes != Now.Minutes
			    || T.Hours != Now.Hours)
				SecondStartUs = Us;
			Now = T;
			Valid = true;
		}
		else
		{
			Valid = false;
		}
	}
}


/***************************************************************************************/
/* A hand: from a little behind the pivot to its length, over the whole face. */
static UiNeedle_t UiClockPage_Hand(const UiClockPage_t *P, uint16_t Position,
                                   int32_t Percent, int32_t Tail, int32_t Width)
{
	return UiNeedle_Place(P->Cx, P->Cy, (float)-Tail,
	                      (float)((P->Radius * Percent) / 100),
	                      (float)Width / 2.0f,
	                      UiGauge_SweepStart(GAUGE_SWEEP_CLOCK),
	                      UiGauge_SweepSpan(GAUGE_SWEEP_CLOCK),
	                      (uint32_t)Position << UI_NEEDLE_Q);
}


/***************************************************************************************/
/* The hands, back to front: hour, minute, second, then the hub over them all.
   None at all when there is no trustworthy time - a clock showing hands is
   claiming to know what time it is. */
static uint32_t UiClockPage_Shapes(const UiClockPage_t *P, UiNeedle_t *Shapes, bool *Red)
{
	UiClockHands_t H;
	uint32_t Frac;
	uint32_t n = 0;

	if (!Valid || P->Digital)
		return 0u;

	Frac = (uint32_t)(((uint64_t)(time_us_32() - SecondStartUs) * UI_CLOCK_FRAC_ONE)
	                  / 1000000u);
	H = UiClock_Hands(Now.Hours, Now.Minutes, Now.Seconds, Frac);

	Shapes[n] = UiClockPage_Hand(P, H.Hour, UI_CLOCK_HOUR_PCT, UI_CLOCK_HOUR_TAIL,
	                             UI_CLOCK_HOUR_WIDTH);
	Red[n++] = false;
	Shapes[n] = UiClockPage_Hand(P, H.Minute, UI_CLOCK_MINUTE_PCT, UI_CLOCK_MINUTE_TAIL,
	                             UI_CLOCK_MINUTE_WIDTH);
	Red[n++] = false;
	Shapes[n] = UiClockPage_Hand(P, H.Second, UI_CLOCK_SECOND_PCT, UI_CLOCK_SECOND_TAIL,
	                             UI_CLOCK_SECOND_WIDTH);
	Red[n++] = true;

	Shapes[n].X0 = P->Cx;
	Shapes[n].Y0 = P->Cy;
	Shapes[n].X1 = P->Cx;
	Shapes[n].Y1 = P->Cy;
	Shapes[n].HalfWidth = (float)UI_CLOCK_HUB_R;
	Red[n++] = true;
	return n;
}


/***************************************************************************************/
static bool UiClockPage_Overlaps(const UiRect_t *A, const UiRect_t *B)
{
	return A->X1 <= B->X2 && B->X1 <= A->X2 && A->Y1 <= B->Y2 && B->Y1 <= A->Y2;
}


/***************************************************************************************/
/* One line of text, centred at Dy below the dial's centre: redrawn only when
   it reads differently, or when forced, putting the face back under the old
   one first. */
static void UiClockPage_Line(UiClockPage_t *P, const DashFont_t *Font, const char *Text,
                             float Dy, char *Held, uint32_t HeldSize, bool *Have,
                             UiRect_t *Rect, bool Force, UiClockDirty_t Dirty, void *Ctx)
{
	int32_t Tx, Ty;
	UiRect_t Ink;

	if (!Force && *Have && strcmp(Text, Held) == 0)
		return;

	if (*Have)
	{
		UiDraw_Restore(P->Surface, Rect);
		if (Dirty != NULL)
			Dirty(Ctx, Rect);
	}

	(void)snprintf(Held, HeldSize, "%s", Text);
	UiText_Centre(Font, Held, P->Cx, P->Cy + Dy, &Tx, &Ty);
	*Have = UiText_Bounds(Font, Held, Tx, Ty, &Ink);
	if (*Have)
	{
		(void)UiDraw_Text(P->Surface, Font, Held, Tx, Ty);
		*Rect = Ink;
		if (Dirty != NULL)
			Dirty(Ctx, &Ink);
	}
}


/***************************************************************************************/
/* The time in figures: small under the analogue hands, or as the whole of the
   digital view with the seconds beneath. */
static void UiClockPage_Digital(UiClockPage_t *P, bool Force, UiClockDirty_t Dirty,
                                void *Ctx)
{
	char Text[sizeof(P->Text)];

	UiClock_Format(Valid, Now.Hours, Now.Minutes, Text, sizeof(Text));

	if (!P->Digital)
	{
		UiClockPage_Line(P, &dash_font_clock_36, Text, (float)UI_CLOCK_DIGITAL_DY,
		                 P->Text, sizeof(P->Text), &P->HaveText, &P->TextRect,
		                 Force, Dirty, Ctx);
		return;
	}

	UiClockPage_Line(P, &dash_font_clock_160, Text, (float)UI_CLOCK_BIG_DY,
	                 P->Text, sizeof(P->Text), &P->HaveText, &P->TextRect,
	                 Force, Dirty, Ctx);

	/* Seconds on their own line, "--" with no trustworthy time for the same
	   reason the hours and minutes read "--:--". */
	{
		char Sec[sizeof(P->Seconds)];

		if (Valid && Now.Seconds < 60u)
			(void)snprintf(Sec, sizeof(Sec), "%02u", (unsigned)Now.Seconds);
		else
			(void)snprintf(Sec, sizeof(Sec), "--");
		UiClockPage_Line(P, &dash_font_clock_36, Sec, (float)UI_CLOCK_SECONDS_DY,
		                 P->Seconds, sizeof(P->Seconds), &P->HaveSeconds,
		                 &P->SecondsRect, Force, Dirty, Ctx);
	}
}


/***************************************************************************************/
void UiClockPage_Load(UiClockPage_t *P, uint8_t Surface, float Cx, float Cy,
                      int32_t Radius, bool Digital)
{
	uint32_t i;

	memset(P, 0, sizeof(*P));
	P->Surface = Surface;
	P->Cx = Cx;
	P->Cy = Cy;
	P->Radius = Radius;
	P->Digital = Digital;

	P->ShapeCount = UiClockPage_Shapes(P, P->Shapes, P->Red);
	for (i = 0; i < P->ShapeCount; i++)
		(void)UiDraw_StrokeIn(Surface, P->Red[i] ? UI_TEXT_RED : UI_TEXT_WHITE,
		                      &P->Shapes[i], UI_DRAW_RED_FULL);

	UiClockPage_Digital(P, true, NULL, NULL);
}


/***************************************************************************************/
/* The hands cross each other, so all of the old ones are put back before any
   of the new ones are drawn - the same rule the g-force page needs. The
   digital time is outside the minute hand's reach and is handled on its own. */
void UiClockPage_Update(UiClockPage_t *P, bool TextDue, UiClockDirty_t Dirty, void *Ctx)
{
	UiNeedle_t Shapes[UI_CLOCKPAGE_SHAPES];
	bool Red[UI_CLOCKPAGE_SHAPES];
	uint32_t n = UiClockPage_Shapes(P, Shapes, Red);
	bool Same = (n == P->ShapeCount);
	bool Touched = false;
	uint32_t i;

	for (i = 0; Same && i < n; i++)
		Same = UiNeedle_Same(&Shapes[i], &P->Shapes[i]);

	if (!Same)
	{
		for (i = 0; i < P->ShapeCount; i++)
		{
			UiRect_t R = UiNeedle_Bounds(&P->Shapes[i]);

			if (P->HaveText && UiClockPage_Overlaps(&R, &P->TextRect))
				Touched = true;
			UiDraw_Restore(P->Surface, &R);
			Dirty(Ctx, &R);
		}
		for (i = 0; i < n; i++)
		{
			UiRect_t R = UiNeedle_Bounds(&Shapes[i]);

			if (P->HaveText && UiClockPage_Overlaps(&R, &P->TextRect))
				Touched = true;
			(void)UiDraw_StrokeIn(P->Surface, Red[i] ? UI_TEXT_RED : UI_TEXT_WHITE,
			                      &Shapes[i], UI_DRAW_RED_FULL);
			Dirty(Ctx, &R);
		}
		memcpy(P->Shapes, Shapes, sizeof(Shapes[0]) * n);
		memcpy(P->Red, Red, sizeof(Red[0]) * n);
		P->ShapeCount = n;
	}

	/* The reading sits where the hands sweep, so a hand that has been over it
	   has just had the FACE put back underneath - which takes the text with
	   it. Redrawing it here rather than only once a minute is what keeps the
	   hands passing behind it instead of erasing pieces of it. */
	if (TextDue || Touched || P->Digital)
		UiClockPage_Digital(P, Touched, Dirty, Ctx);
}
