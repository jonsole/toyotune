/*
 * pages.h
 *
 * What each gauge face shows, as data.
 *
 * Touch is the interaction model, not spare hardware: swiping left and right
 * cycles a node through this shared list. So the node identity picks the
 * STARTUP page, not the only page - all three nodes carry the same list and
 * differ only in where they begin. That turns three fixed faces into three
 * independently steerable windows onto a larger set.
 *
 * Adding a gauge is a row; adding a page is a table; adding a node is a
 * startup index. If you find yourself writing node-2-specific code, something
 * has gone wrong.
 */

#ifndef PAGES_H_
#define PAGES_H_

#include <stdbool.h>
#include <stddef.h>	/* NULL, for the tick-label pointers below */
#include <stdint.h>

#include "signals.h"

typedef enum
{
	WIDGET_GAUGE,		/* ticks, numbers and a swinging needle - a real dial */
	WIDGET_DIAL,		/* filled arc, no ticks; cheaper than a gauge */
	WIDGET_ARC,			/* partial ring, good for a secondary quantity */
	WIDGET_NUMERIC,		/* plain value and unit */
	WIDGET_BARGRAPH,	/* horizontal bar, for per-cylinder comparisons */
	WIDGET_GRAPH,		/* rolling trace against time */
	WIDGET_GFORCE,		/* friction circle from the node's own accelerometer */
	WIDGET_CLOCK,		/* hands, from the RTC and the time announced on the bus */
	WIDGET_CLOCK_DIGITAL	/* the same time, in large figures */
} WidgetType_t;


/* How much of the dial a gauge sweeps. A page may carry one FULL gauge, or a
   TOP and a BOTTOM sharing the same centre - two instruments in one face. The
   angles are in ui_gauge.h. */
typedef enum
{
	GAUGE_SWEEP_FULL = 0,	/* 270 degrees, lower left round to lower right */
	GAUGE_SWEEP_TOP,	/* the upper half, left to right over the top */
	GAUGE_SWEEP_BOTTOM,	/* the lower half, left to right under the bottom */
	GAUGE_SWEEP_CLOCK	/* all the way round, twelve at the top - a clock */
} GaugeSweep_t;


typedef struct
{
	WidgetType_t Type;
	SignalId_t Signal;
	int32_t Min;		/* sweep range; may be narrower than the descriptor's */
	int32_t Max;
	uint8_t X, Y, W, H;	/* percent of the panel, so the layout is resolution
						   independent - the same table serves the 1.43" and
						   1.75" panels, which share 466x466 */

	/* WIDGET_GAUGE only: the numbers printed around the dial, NULL terminated.
	   One string per major tick, and the minor ticks are filled in between.

	   Deliberately text rather than derived from Min and Max. A tachometer is
	   marked 0 to 8, not 0 to 8000, and a boost gauge in kPa wants round
	   hundreds however the signal is scaled - so what is written on the face
	   stays a presentation choice and cannot drift into the needle
	   arithmetic. NULL on any other widget. */
	const char *const *Ticks;

	/* WIDGET_GAUGE only: the legend painted under the dial's centre, as the
	   MR2 prints "x1000r/min". NULL for none. */
	const char *Legend;

	/* WIDGET_GAUGE only: where the red warning band starts, in the element's
	   own units; it runs to Max. Painted on the face and nothing more - whether
	   the gauge itself turns to its warning state is UiModel_SignalWarning()'s
	   decision, which this does not feed.

	   A band is painted whenever this lies strictly inside Min..Max, so 0
	   means "no band" only on a scale that starts at or above zero. On one
	   that goes negative - the air temperatures - 0 is a real place on the
	   dial and paints red from there; use PAGES_NO_BAND. */
	int32_t BandFrom;

	/* WIDGET_GAUGE only: how much of the dial it takes. Zero is FULL, so a
	   table that does not mention it gets the ordinary dial. */
	GaugeSweep_t Sweep;

	/* How the reading is printed, when the signal's own units are not the ones
	   the face is marked in - boost read in bar from a MAP sensor in kPa.
	   NULL prints the signal as its descriptor says. Returns Out. */
	const char *(*Format)(int32_t Value, char *Out, uint32_t OutSize);
} FaceElement_t;


/* A page, and the other way of looking at it.

   Swiping sideways moves between pages; swiping up or down flips the page
   between its two VIEWS - a needle gauge and a strip chart of the same
   readings, or an analogue clock and a digital one. The second view is part
   of the page rather than a page of its own, so a reading has one place in the
   list whichever way it is being shown, and the page remembers which view it
   was left on. AltElements is NULL for a page with only one view. */
typedef struct
{
	const char *Name;
	const FaceElement_t *Elements;
	uint8_t ElementCount;
	const FaceElement_t *AltElements;
	uint8_t AltElementCount;
} FacePage_t;

#define PAGES_VIEWS		(2u)


/* BandFrom for "no red band", whatever the scale's range. */
#define PAGES_NO_BAND		(INT32_MAX)

/* An upper bound on the page list, for anything that keeps state per page -
   the strip chart's history. Checked against PageCount by the host tests, so
   adding pages past it cannot go unnoticed. */
#define PAGES_MAX		(8u)

extern const FacePage_t Pages[];
extern const uint8_t PageCount;

/* Which page each node shows at power-on, indexed by node identity. */
extern const uint8_t StartupPage[];

/* Page selection. Kept here rather than in the display layer so it can be
   tested without a panel, and so the warning takeover below cannot be
   forgotten by whoever writes the rendering. */
extern void Pages_Init(uint8_t NodeId, uint8_t RestoredPage);
extern uint8_t Pages_Current(void);
extern void Pages_Next(void);
extern void Pages_Previous(void);

/* The page Pages_Next() (Direction > 0) or Pages_Previous() (< 0) would
   select, without selecting it - what a swipe in progress is bringing in. */
extern uint8_t Pages_Neighbour(int Direction);

/* Views. Each page remembers its own, so swiping away from a trace and back
   finds the trace again. The warning takeover has one view only. */
extern bool Pages_HasAlt(uint8_t Page);
extern uint8_t Pages_ViewOf(uint8_t Page);
extern void Pages_Flip(void);		/* the selected page's other view */

/* The selection, for remembering it across a power cycle. Nothing calls these
   yet: the store they were written for was the RP2350's own flash, and that
   is being replaced by an I2C EEPROM on the carrier - see PLAN.md 4.14a.
   Restore ignores anything it cannot make sense of, because a record from
   another build must never leave a node unable to start. */
extern void Pages_Restore(uint8_t Page, const uint8_t *Views);
extern void Pages_Snapshot(uint8_t *Page, uint8_t *Views);

/* A view's elements: 0 is the page's own, 1 its alternative. NULL, with
   Count 0, for a view the page does not have. */
extern const FaceElement_t *Pages_Elements(uint8_t Page, uint8_t View, uint8_t *Count);

/* True when a fault should take the whole face over regardless of what the
   driver selected. A driver must not be able to swipe away from a fault, so
   this outranks the selection rather than being another page in the list. */
extern bool Pages_WarningActive(uint32_t NowMs);

/* The page to actually draw: the warning page while a fault stands, the
   selected one otherwise. */
extern uint8_t Pages_Effective(uint32_t NowMs);

/* Knock retard beyond this many hundredths of a degree raises the warning.
   Deliberately well above the noise a healthy engine shows. */
#define PAGES_KNOCK_WARN_DEG100		(300)

#endif /* PAGES_H_ */
