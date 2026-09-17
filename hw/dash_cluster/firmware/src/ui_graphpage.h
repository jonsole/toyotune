/*
 * ui_graphpage.h
 *
 * The strip-chart page: the history collected for every graph page, and one
 * page's plot and readings drawn over the pre-rendered frame.
 *
 * History is collected for ALL graph pages, whichever one is on the glass, so
 * swiping to a trace shows the last ten seconds rather than starting blank.
 * The view - what has been drawn where - belongs to a page, since during a
 * swipe a page may be drawn into either surface.
 */
#ifndef UI_GRAPHPAGE_H_
#define UI_GRAPHPAGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "pages.h"
#include "ui_graph.h"
#include "ui_needle.h"

/* Traces per graph page. */
#define UI_GRAPHPAGE_TRACES	(2u)

/* How long the plot holds, and so how long a column stands for. */
#define UI_GRAPHPAGE_WINDOW_MS	(10000u)
#define UI_GRAPHPAGE_STEP_US	((UI_GRAPHPAGE_WINDOW_MS * 1000u) / UI_GRAPH_COLUMNS)

typedef void (*UiGraphDirty_t)(void *Context, const UiRect_t *Rect);

typedef struct
{
	uint8_t Surface;
	uint8_t Page;
	UiRect_t Interior;		/* the plot, inside its frame */
	uint32_t TraceCount;
	uint32_t Rows;			/* grid bands, from the scale's labels */
	uint32_t DrawnStep;		/* the history step this plot was drawn at */

	bool HaveText[UI_GRAPHPAGE_TRACES];
	char Text[UI_GRAPHPAGE_TRACES][16];
	UiRect_t TextRect[UI_GRAPHPAGE_TRACES];
} UiGraphPage_t;

extern void UiGraphPage_Init(void);

/* Collect a sample for every graph page, if a column's worth of time has
   passed. Once a frame, whichever page is showing. */
extern void UiGraphPage_Sample(uint32_t NowMs, uint32_t FrameUs);

/* Is this page a graph page? */
extern bool UiGraphPage_Has(uint8_t Page);

/* A view of a graph page, in a surface whose face is already loaded. Draws the
   plot and the readings. */
extern void UiGraphPage_Load(UiGraphPage_t *View, uint8_t Surface, uint8_t Page,
                             float Cx, float Cy, uint32_t NowMs);

/* Redraw what has changed - the plot when the history has moved on, the
   readings when they read differently - reporting every rectangle touched. */
extern void UiGraphPage_Update(UiGraphPage_t *View, uint32_t NowMs, bool TextDue,
                               UiGraphDirty_t Dirty, void *Context);

#endif /* UI_GRAPHPAGE_H_ */
