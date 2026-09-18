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
 * ui_draw.c is left with the drawing: spans, glyphs and pixels, none of which
 * has an opinion about what a reading means. The renderer is small enough to
 * read and confirm by eye; the judgements are small enough to test
 * exhaustively. Neither would be true of one file that did both.
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

/* One full out-and-back of the self-test sweep, in milliseconds: half of it
   climbing to full scale, half returning. A gauge with no reading sweeps
   instead of sitting at zero - see the comment in UiModel_Widget(). */
#define UI_SWEEP_PERIOD_MS	(2400u)

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

/* Readings made on the display core itself - the accelerometer's - which are
   not in the signal store because the store has a single writer on the other
   core. Set once a frame; a widget on one of these signals reads from here
   instead, with the same staleness rule, so a graph of them behaves exactly
   like a graph of anything else. Ignored for any signal the store carries. */
#define UI_MODEL_LOCAL_STALE_MS		(500u)
extern void UiModel_SetLocal(SignalId_t Signal, int32_t Value, uint32_t NowMs);

/* True when this signal's value is in its warning band. Exposed separately
   because a page may want to colour a whole face, not just one widget. */
extern bool UiModel_SignalWarning(SignalId_t Signal, int32_t Value);

/* NEEDLE SMOOTHING.
 *
 * A reading changes 50 times a second - the FAST telemetry tier is 20 ms - and
 * the panel shows 60 frames, so a needle drawn straight from the reading holds
 * still on some frames and jumps on others. Easing it towards the reading
 * instead moves it a little on every frame, and gives it the damped motion of
 * a mechanical gauge.
 *
 * First order, with time constant UI_NEEDLE_TAU_US, stepped by the real frame
 * interval - so a late frame moves the needle further rather than slowing the
 * whole animation down. The step is backward Euler, dt / (tau + dt), which can
 * never overshoot or oscillate however long a frame takes.
 *
 * Positions are 0..UI_POSITION_MAX with UI_NEEDLE_Q fractional bits: in whole
 * permille the needle would stall a unit short of a slowly moving target, and
 * a unit is a third of a pixel at the tip. */
#define UI_NEEDLE_Q			(8)
#define UI_NEEDLE_TAU_US		(40000u)

/* A gap longer than this - a page just built, core 1 held up - is not an
   animation to catch up on. The needle goes straight to the reading. */
#define UI_NEEDLE_JUMP_US		(250000u)

/* One frame's step from CurrentQ towards Target (0..UI_POSITION_MAX), given
   how long the frame took. Returns the new position, UI_NEEDLE_Q fractional
   bits. */
extern uint32_t UiModel_NeedleStep(uint32_t CurrentQ, uint16_t Target, uint32_t DtUs);

#endif /* UI_MODEL_H_ */
