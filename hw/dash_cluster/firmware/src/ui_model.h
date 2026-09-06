/*
 * ui_model.h
 *
 * What a widget should be showing right now, decided independently of LVGL.
 *
 * WHY THIS IS SEPARATE FROM THE LVGL CODE
 *
 * The interesting decisions in a gauge are not the drawing: they are what
 * counts as stale, where a needle sits when the value is off-scale, whether a
 * reading is in its warning band, and what a widget shows before any frame has
 * arrived. Those are exactly the things that are wrong quietly - a needle
 * pinned at zero looks like an idling engine, not like a dead bus.
 *
 * So they are decided here, in code that builds and is tested on a host, and
 * ui_lvgl.c is left as a mechanical translation into lv_arc/lv_label/lv_bar
 * calls. The binding is small enough to read and confirm by eye; the
 * judgements are small enough to test exhaustively. Neither would be true of
 * one file that did both.
 */

#ifndef UI_MODEL_H_
#define UI_MODEL_H_

#include <stdbool.h>
#include <stdint.h>

#include "pages.h"
#include "signals.h"

/* How a widget should be drawn, beyond its value. Colour is deliberately not
   decided here - that is a theme's business - but the state that drives it
   is. */
typedef enum
{
	UI_STATE_NORMAL,
	UI_STATE_WARNING,	/* in its warning band, but live */
	UI_STATE_STALE,		/* last value known, but too old to trust */
	UI_STATE_NODATA		/* nothing has ever arrived for this signal */
} UiState_t;

/* Needle and bar positions are given as permille of the element's own range,
   so the drawing code never has to know a signal's units or do the scaling
   twice. Clamped to 0..1000: a value off the end of the scale pins the needle
   rather than wrapping it round, which would read as a plausible low value. */
#define UI_POSITION_MAX		(1000)

typedef struct
{
	UiState_t State;
	int32_t Value;			/* raw store value, for callers that want it */
	uint16_t Position;		/* 0..UI_POSITION_MAX within the element range */
	bool OffScale;			/* value was outside the element's range */
	char Text[16];			/* formatted value, "--" when there is no data */
	const char *Unit;
	const char *Label;
} UiWidget_t;


/* Warning bands. These are display thresholds, not ECU limits - they decide
   when a gauge turns red, and nothing else. Kept here so all three nodes
   agree, and so changing one is a one-line edit rather than a hunt. */
#define UI_WARN_ECT_C100		(10500)		/* 105.00 degC */
#define UI_WARN_BATTERY_LOW_V100	(1150)	/* 11.50 V */
#define UI_WARN_INJ_DUTY_PCT100		(8500)	/* 85.00 % */
#define UI_WARN_KNOCK_DEG100		(300)	/* 3.00 deg, matches the takeover */

/* Build the display state for one element of one page. NowMs drives the
   staleness decision, so a caller must pass a real clock rather than zero. */
extern UiWidget_t UiModel_Widget(const FaceElement_t *Element, uint32_t NowMs);

/* True when this signal's value is in its warning band. Exposed separately
   because a page may want to colour a whole face, not just one widget. */
extern bool UiModel_SignalWarning(SignalId_t Signal, int32_t Value);

#endif /* UI_MODEL_H_ */
