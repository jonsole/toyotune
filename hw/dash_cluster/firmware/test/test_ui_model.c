/*
 * test_ui_model.c
 *
 * Host tests for the UI decisions - the half of the display layer that can be
 * wrong without looking wrong.
 *
 * A needle at zero, a needle pinned at full scale and a needle with no data
 * behind it all look similar on a panel, and only one of them means the
 * engine is idling. These assert that the model tells them apart before any
 * pixels are involved.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pages.h"
#include "signal_store.h"
#include "signals.h"
#include "telemetry.h"
#include "ui_model.h"

extern int UiTests_Run(int *Checks, int *Failures);

static int Checks;
static int Failures;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		Checks++;                                                             \
		if (!(cond)) {                                                        \
			Failures++;                                                       \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
			printf(__VA_ARGS__);                                              \
			printf("\n");                                                     \
		}                                                                     \
	} while (0)


static FaceElement_t MakeElement(SignalId_t Signal, int32_t Min, int32_t Max)
{
	FaceElement_t E;

	memset(&E, 0, sizeof(E));
	E.Type = WIDGET_DIAL;
	E.Signal = Signal;
	E.Min = Min;
	E.Max = Max;
	E.W = 100;
	E.H = 100;
	return E;
}


/***************************************************************************************/
static void TestNoData(void)
{
	FaceElement_t E = MakeElement(SIGNAL_RPM, 0, 8000);
	UiWidget_t W;

	printf("ui - no data yet\n");
	SignalStore_Init();

	W = UiModel_Widget(&E, 0);
	CHECK(W.State == UI_STATE_NODATA, "state should be NODATA");
	CHECK(strcmp(W.Text, "--") == 0, "text should be --, got %s", W.Text);

	/* With no reading the needle sweeps end to end rather than parking, so
	   walk one period: bottom, quarter, top, three-quarter, bottom. */
	CHECK(W.Position == 0, "sweep starts at the bottom");
	CHECK(UiModel_Widget(&E, UI_SWEEP_PERIOD_MS / 4u).Position == UI_POSITION_MAX / 2,
	      "a quarter through, half way up");
	CHECK(UiModel_Widget(&E, UI_SWEEP_PERIOD_MS / 2u).Position == UI_POSITION_MAX,
	      "half way through, full scale");
	CHECK(UiModel_Widget(&E, (UI_SWEEP_PERIOD_MS * 3u) / 4u).Position == UI_POSITION_MAX / 2,
	      "three quarters through, half way back down");
	CHECK(UiModel_Widget(&E, UI_SWEEP_PERIOD_MS).Position == 0,
	      "a full period returns to the bottom");

	/* And it repeats rather than running once. */
	CHECK(UiModel_Widget(&E, UI_SWEEP_PERIOD_MS + (UI_SWEEP_PERIOD_MS / 2u)).Position
	      == UI_POSITION_MAX, "the sweep repeats");

	/* Sweeping must never be mistakable for a reading. */
	CHECK(strcmp(UiModel_Widget(&E, UI_SWEEP_PERIOD_MS / 2u).Text, "--") == 0,
	      "a sweeping needle still reads --");

	/* This is the point of the test: a gauge with no data must not be
	   indistinguishable from a gauge reading zero. Once a reading lands the
	   sweep stops dead, whatever the clock is doing. */
	SignalStore_Set(SIGNAL_RPM, 0, 1000);
	W = UiModel_Widget(&E, 1000);
	CHECK(W.State == UI_STATE_NORMAL, "a real zero is NORMAL, not NODATA");
	CHECK(strcmp(W.Text, "0") == 0, "a real zero prints as 0, got %s", W.Text);
	CHECK(W.Position == 0, "a real zero parks the needle, sweep or no sweep");

	/* Stale data holds its last reading rather than resuming the sweep: a
	   needle that started moving when the bus died would look like live data
	   from a dead link. */
	SignalStore_Set(SIGNAL_RPM, 4000, 1000);
	W = UiModel_Widget(&E, 1000u + (UI_SWEEP_PERIOD_MS * 10u));
	CHECK(W.State == UI_STATE_STALE, "an old reading is STALE");
	CHECK(W.Position == UI_POSITION_MAX / 2, "stale holds its value, it does not sweep");
}


static void TestPositionScaling(void)
{
	FaceElement_t E = MakeElement(SIGNAL_RPM, 0, 8000);
	UiWidget_t W;

	printf("ui - needle position\n");
	SignalStore_Init();

	SignalStore_Set(SIGNAL_RPM, 0, 1000);
	CHECK(UiModel_Widget(&E, 1000).Position == 0, "bottom of scale");

	SignalStore_Set(SIGNAL_RPM, 4000, 1000);
	W = UiModel_Widget(&E, 1000);
	CHECK(W.Position == 500, "midpoint should be 500, got %u", W.Position);

	SignalStore_Set(SIGNAL_RPM, 8000, 1000);
	CHECK(UiModel_Widget(&E, 1000).Position == UI_POSITION_MAX, "top of scale");

	/* Off-scale must pin, not wrap. A needle that wraps past full scale back
	   to zero reads as an idling engine at 9000 rpm. */
	SignalStore_Set(SIGNAL_RPM, 12000, 1000);
	W = UiModel_Widget(&E, 1000);
	CHECK(W.Position == UI_POSITION_MAX, "over-range pins at full scale");
	CHECK(W.OffScale, "and is reported as off-scale");

	/* The intermediate multiply must not overflow on a wide range. */
	E = MakeElement(SIGNAL_INJ_PW, 0, 25000);
	SignalStore_Set(SIGNAL_INJ_PW, 25000, 1000);
	CHECK(UiModel_Widget(&E, 1000).Position == UI_POSITION_MAX,
	      "a wide range must not overflow the scaling");
}


static void TestNegativeRange(void)
{
	FaceElement_t E = MakeElement(SIGNAL_ECT, -4000, 12000);
	UiWidget_t W;

	printf("ui - ranges spanning zero\n");
	SignalStore_Init();

	SignalStore_Set(SIGNAL_ECT, -4000, 1000);
	CHECK(UiModel_Widget(&E, 1000).Position == 0, "bottom of a negative range");

	SignalStore_Set(SIGNAL_ECT, 4000, 1000);
	W = UiModel_Widget(&E, 1000);
	CHECK(W.Position == 500, "midpoint of -4000..12000 is 4000, got %u", W.Position);

	SignalStore_Set(SIGNAL_ECT, -5000, 1000);
	W = UiModel_Widget(&E, 1000);
	CHECK(W.Position == 0 && W.OffScale, "under-range pins at zero");
}


static void TestStalenessOutranksWarning(void)
{
	FaceElement_t E = MakeElement(SIGNAL_ECT, -4000, 12000);
	UiWidget_t W;

	printf("ui - staleness outranks a warning band\n");
	SignalStore_Init();

	/* Hot enough to warn. */
	SignalStore_Set(SIGNAL_ECT, UI_WARN_ECT_C100 + 100, 10000);
	W = UiModel_Widget(&E, 10000);
	CHECK(W.State == UI_STATE_WARNING, "over-temperature warns while fresh");

	/* Once it goes stale it must stop asserting the engine is too hot: the
	   reading it would be asserting from may be minutes out of date. MEDIUM
	   is 100 ms, so three periods is 300 ms. */
	W = UiModel_Widget(&E, 10000 + 400);
	CHECK(W.State == UI_STATE_STALE, "stale outranks warning, got %d", (int)W.State);
	CHECK(strcmp(W.Text, "106.00") == 0, "the last value is still shown, got %s",
	      W.Text);
}


static void TestWarningBands(void)
{
	printf("ui - warning bands\n");

	CHECK(!UiModel_SignalWarning(SIGNAL_ECT, UI_WARN_ECT_C100 - 1), "just under");
	CHECK(UiModel_SignalWarning(SIGNAL_ECT, UI_WARN_ECT_C100), "at threshold");

	/* Battery warns LOW, which is the opposite direction to every other band
	   here - easy to get backwards, so asserted explicitly. */
	CHECK(UiModel_SignalWarning(SIGNAL_BATTERY, 1000), "a flat battery warns");
	CHECK(!UiModel_SignalWarning(SIGNAL_BATTERY, 1400), "a healthy one does not");

	CHECK(UiModel_SignalWarning(SIGNAL_INJ_DUTY, 9000), "high duty warns");
	CHECK(UiModel_SignalWarning(SIGNAL_KNOCK_CYL2, UI_WARN_KNOCK_DEG100), "knock warns");
	CHECK(UiModel_SignalWarning(SIGNAL_ERROR_FLAGS1, 1), "any fault bit warns");
	CHECK(!UiModel_SignalWarning(SIGNAL_ERROR_FLAGS1, 0), "no bits, no warning");
	CHECK(!UiModel_SignalWarning(SIGNAL_TPS_RAW, 65535), "a raw signal has no band");
}


/* Every element of every page must produce a drawable widget - no crash, no
   empty label - even with an empty store. That is the state at power-on. */
static void TestEveryElementRenders(void)
{
	uint8_t p, e;

	printf("ui - every page element renders with an empty store\n");
	SignalStore_Init();

	for (p = 0; p < PageCount; p++)
	{
		for (e = 0; e < Pages[p].ElementCount; e++)
		{
			UiWidget_t W = UiModel_Widget(&Pages[p].Elements[e], 5000);

			CHECK(W.Label != NULL && W.Label[0], "page %u element %u has no label",
			      p, e);
			CHECK(W.Unit != NULL, "page %u element %u has no unit", p, e);
			CHECK(W.Text[0] != '\0', "page %u element %u has no text", p, e);
			CHECK(W.Position <= UI_POSITION_MAX,
			      "page %u element %u position %u out of range", p, e, W.Position);
		}
	}
}


/***************************************************************************************/
static void TestNeedleSmoothing(void)
{
	const uint32_t Frame = 16700u;	/* one 60 Hz panel scan */
	const uint32_t Full = (uint32_t)UI_POSITION_MAX << UI_NEEDLE_Q;
	uint32_t Q, Prev, A, B;
	int k, Frames;

	printf("ui - needle smoothing\n");

	CHECK(UiModel_NeedleStep(Full / 2u, UI_POSITION_MAX / 2, Frame) == Full / 2u,
	      "a needle already on its reading stays put");

	/* Climbs without overshoot, every frame moving it, and arrives. */
	Q = 0;
	Frames = 0;
	for (k = 0; k < 200 && Q != Full; k++)
	{
		Prev = Q;
		Q = UiModel_NeedleStep(Q, UI_POSITION_MAX, Frame);
		CHECK(Q > Prev, "frame %d did not move a needle that is short of its reading", k);
		CHECK(Q <= Full, "frame %d overshot full scale: %u", k, Q);
		Frames++;
	}
	CHECK(Q == Full, "never reached full scale");
	CHECK(Frames < 30, "took %d frames to cross the dial - too sluggish", Frames);
	CHECK(Frames > 4, "took %d frames to cross the dial - no smoothing at all", Frames);

	/* And the same falling. */
	for (k = 0; k < 200 && Q != 0; k++)
	{
		Prev = Q;
		Q = UiModel_NeedleStep(Q, 0, Frame);
		CHECK(Q < Prev, "frame %d did not move a falling needle", k);
	}
	CHECK(Q == 0, "never returned to zero");

	/* Steps by the real frame time: ten 60 Hz frames and five frames twice as
	   long cover about the same ground, rather than a slow frame rate slowing
	   the needle down with it. */
	A = 0;
	for (k = 0; k < 10; k++)
		A = UiModel_NeedleStep(A, UI_POSITION_MAX, Frame);
	B = 0;
	for (k = 0; k < 5; k++)
		B = UiModel_NeedleStep(B, UI_POSITION_MAX, Frame * 2u);
	CHECK((A > B ? A - B : B - A) < Full / 20u,
	      "frame time not honoured: 10x16.7ms reached %u, 5x33.4ms reached %u", A, B);

	/* A long stall - or a page just built - is not caught up gradually. */
	CHECK(UiModel_NeedleStep(0, UI_POSITION_MAX, UI_NEEDLE_JUMP_US) == Full,
	      "a long gap should put the needle straight on its reading");

	/* An out-of-range target is clamped, as positions are everywhere else. */
	CHECK(UiModel_NeedleStep(Full, UI_POSITION_MAX + 50, Frame) == Full,
	      "target beyond full scale should clamp");
}


/***************************************************************************************/
/* The boost-over-AFR page: its two gauges, their sweeps, and the readings
   printed in the face's own units rather than the signals'. */
static void TestSplitPage(void)
{
	const FacePage_t *P = NULL;
	const FaceElement_t *Boost = NULL, *Afr = NULL;
	UiWidget_t W;
	uint8_t i;

	for (i = 0; i < PageCount; i++)
		if (strcmp(Pages[i].Name, "Boost / AFR") == 0)
			P = &Pages[i];
	CHECK(P != NULL, "the boost / AFR page should exist");
	if (P == NULL)
		return;

	for (i = 0; i < P->ElementCount; i++)
	{
		if (P->Elements[i].Signal == SIGNAL_MAP)
			Boost = &P->Elements[i];
		if (P->Elements[i].Signal == SIGNAL_AFR)
			Afr = &P->Elements[i];
	}
	CHECK(Boost != NULL && Boost->Sweep == GAUGE_SWEEP_TOP, "boost is the top half");
	CHECK(Afr != NULL && Afr->Sweep == GAUGE_SWEEP_BOTTOM, "AFR is the bottom half");
	if (Boost == NULL || Afr == NULL)
		return;

	SignalStore_Init();

	/* Boost in bar from absolute pressure in tenths of a kPa: atmospheric is
	   zero, with the sign and the rounding right either side of it. */
	{
		static const struct { int32_t Kpa10; const char *Text; } Cases[] =
		{
			{ 1013, "0.00" },	/* atmospheric */
			{ 2263, "1.25" },
			{ 2513, "1.50" },	/* full scale */
			{  513, "-0.50" },
			{   13, "-1.00" },	/* bottom of scale */
			{ 1008, "-0.01" },	/* half a hundredth below rounds away */
			{ 1017, "0.00" },	/* under half a hundredth above */
			{ 1018, "0.01" },
		};
		size_t n;

		for (n = 0; n < sizeof(Cases) / sizeof(Cases[0]); n++)
		{
			SignalStore_Set(SIGNAL_MAP, Cases[n].Kpa10, 1000);
			W = UiModel_Widget(Boost, 1000);
			CHECK(strcmp(W.Text, Cases[n].Text) == 0,
			      "MAP %d should read %s bar, got %s",
			      (int)Cases[n].Kpa10, Cases[n].Text, W.Text);
		}
	}

	/* The needle: atmospheric sits at 1 bar of the 2.5 bar sweep. */
	SignalStore_Set(SIGNAL_MAP, 1013, 1000);
	W = UiModel_Widget(Boost, 1000);
	CHECK(W.Position == 400, "atmospheric should be 400 of the sweep, got %u", W.Position);

	/* AFR to one decimal, rounded. */
	{
		static const struct { int32_t Afr100; const char *Text; } Cases[] =
		{
			{ 1470, "14.7" }, { 1474, "14.7" }, { 1475, "14.8" },
			{ 1000, "10.0" }, { 2000, "20.0" }, { 1150, "11.5" },
		};
		size_t n;

		for (n = 0; n < sizeof(Cases) / sizeof(Cases[0]); n++)
		{
			SignalStore_Set(SIGNAL_AFR, Cases[n].Afr100, 1000);
			W = UiModel_Widget(Afr, 1000);
			CHECK(strcmp(W.Text, Cases[n].Text) == 0, "AFR %d should read %s, got %s",
			      (int)Cases[n].Afr100, Cases[n].Text, W.Text);
		}
	}

	SignalStore_Set(SIGNAL_AFR, 1500, 1000);
	CHECK(UiModel_Widget(Afr, 1000).Position == UI_POSITION_MAX / 2,
	      "15.0 is the middle of a 10-20 sweep");

	/* The reading's text must fit what UiWidget_t carries, at both extremes. */
	SignalStore_Set(SIGNAL_MAP, -99999, 1000);
	W = UiModel_Widget(Boost, 1000);
	CHECK(strlen(W.Text) < sizeof(W.Text) - 1u, "a wild MAP reading still fits: %s", W.Text);
}


/***************************************************************************************/
int UiTests_Run(int *OutChecks, int *OutFailures)
{
	Checks = 0;
	Failures = 0;

	TestNoData();
	TestPositionScaling();
	TestNegativeRange();
	TestStalenessOutranksWarning();
	TestWarningBands();
	TestEveryElementRenders();
	TestNeedleSmoothing();
	TestSplitPage();

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
