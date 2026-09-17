/*
 * ui_graph.h
 *
 * The strip chart: a history per trace, one sample per plot column, and the
 * plot drawn from it into an 8-bit buffer. No hardware and no display
 * dependency, so it is host-tested; ui_graphpage.c puts it on a page.
 *
 * The chart scrolls: the newest sample is the rightmost column, and every new
 * sample moves the whole trace one column left. The plot's interior is
 * redrawn from nothing each time - background, grid, traces - rather than
 * shifted, because it is small, the buffer is in SRAM, and a redraw cannot
 * leave anything behind.
 */
#ifndef UI_GRAPH_H_
#define UI_GRAPH_H_

#include <stdbool.h>
#include <stdint.h>

#include "ui_needle.h"

/* Plot columns: one sample each. With the window in ui_gauge.h this is the
   time a column stands for. */
#define UI_GRAPH_COLUMNS	(300u)

/* A column with no reading - the signal had never arrived, or had gone stale.
   Drawn as a gap, so a trace never claims a value it did not have. */
#define UI_GRAPH_GAP		(0xFFFFu)

typedef struct
{
	uint16_t Pos[UI_GRAPH_COLUMNS];	/* 0..UI_POSITION_MAX, or UI_GRAPH_GAP */
	uint16_t Next;			/* where the next sample goes */
	uint16_t Count;			/* samples held, up to UI_GRAPH_COLUMNS */
} UiTrace_t;

extern void UiTrace_Clear(UiTrace_t *Trace);
extern void UiTrace_Push(UiTrace_t *Trace, uint16_t Pos);

/* The sample Age columns ago, 0 being the newest. UI_GRAPH_GAP beyond what is
   held. */
extern uint16_t UiTrace_Get(const UiTrace_t *Trace, uint32_t Age);

/* The palette indices the plot is drawn with. */
typedef struct
{
	uint8_t Background;
	uint8_t Grid;
	uint8_t Trace[2];
} UiGraphInk_t;

/* How thick a trace is, in pixels. */
#define UI_GRAPH_TRACE_PX	(2)

/* Draw the plot into Interior - which must be UI_GRAPH_COLUMNS wide - with
   GridRows horizontal bands and GridCols vertical ones, and up to two traces,
   the first drawn on top. Everything inside Interior is overwritten; nothing
   outside it is touched. */
extern void UiGraph_Draw(uint8_t *Buffer, uint32_t Stride, const UiRect_t *Interior,
                         const UiTrace_t *const *Traces, uint32_t TraceCount,
                         const UiGraphInk_t *Ink, uint32_t GridRows, uint32_t GridCols);

/* The row a position is plotted on, within Interior. */
extern int32_t UiGraph_Row(const UiRect_t *Interior, uint16_t Pos);

#endif /* UI_GRAPH_H_ */
