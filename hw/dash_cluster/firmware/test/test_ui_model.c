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

	W = UiModel_Widget(&E, 1000);
	CHECK(W.State == UI_STATE_NODATA, "state should be NODATA");
	CHECK(strcmp(W.Text, "--") == 0, "text should be --, got %s", W.Text);
	CHECK(W.Position == 0, "needle parks at zero");

	/* This is the point of the test: a gauge with no data must not be
	   indistinguishable from a gauge reading zero. */
	SignalStore_Set(SIGNAL_RPM, 0, 1000);
	W = UiModel_Widget(&E, 1000);
	CHECK(W.State == UI_STATE_NORMAL, "a real zero is NORMAL, not NODATA");
	CHECK(strcmp(W.Text, "0") == 0, "a real zero prints as 0, got %s", W.Text);
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

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
