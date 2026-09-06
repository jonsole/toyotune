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
#include <stdint.h>

#include "signals.h"

typedef enum
{
	WIDGET_DIAL,		/* sweeping needle, the main event on a page */
	WIDGET_ARC,			/* partial ring, good for a secondary quantity */
	WIDGET_NUMERIC,		/* plain value and unit */
	WIDGET_BARGRAPH,	/* horizontal bar, for per-cylinder comparisons */
	WIDGET_GRAPH		/* rolling trace against time */
} WidgetType_t;


typedef struct
{
	WidgetType_t Type;
	SignalId_t Signal;
	int32_t Min;		/* sweep range; may be narrower than the descriptor's */
	int32_t Max;
	uint8_t X, Y, W, H;	/* percent of the panel, so the layout is resolution
						   independent - the same table serves the 1.43" and
						   1.75" panels, which share 466x466 */
} FaceElement_t;


typedef struct
{
	const char *Name;
	const FaceElement_t *Elements;
	uint8_t ElementCount;
} FacePage_t;


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
