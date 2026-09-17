/*
 * ui_graphpage.c - the strip-chart page. See ui_graphpage.h.
 */

#include "ui_graphpage.h"

#include <stdio.h>
#include <string.h>

#include "dash_font.h"
#include "ui_draw.h"
#include "ui_gauge.h"
#include "ui_model.h"
#include "ui_text.h"

extern const DashFont_t dash_font_value_56;

/* Vertical grid bands: one per two seconds of the window. */
#define UI_GRAPHPAGE_COLS	(5u)

/* One history per graph page, so a page's trace is there when it is swiped
   to. Pages that are not graphs simply never get a sample. */
static UiTrace_t	History[PAGES_MAX][UI_GRAPHPAGE_TRACES];

/* Counts up one per column of history taken - what tells a view its plot is
   out of date. */
static uint32_t		Step;
static uint32_t		DueUs;


/***************************************************************************************/
/* A page's graph elements, in order. Returns how many. */
static uint32_t UiGraphPage_Elements(uint8_t Page, const FaceElement_t **Out)
{
	uint32_t n = 0;
	uint8_t e;

	for (e = 0; e < Pages[Page].ElementCount && n < UI_GRAPHPAGE_TRACES; e++)
		if (Pages[Page].Elements[e].Type == WIDGET_GRAPH)
			Out[n++] = &Pages[Page].Elements[e];

	return n;
}

bool UiGraphPage_Has(uint8_t Page)
{
	const FaceElement_t *Elements[UI_GRAPHPAGE_TRACES];

	return Page < PageCount && UiGraphPage_Elements(Page, Elements) != 0u;
}


/***************************************************************************************/
void UiGraphPage_Init(void)
{
	uint8_t p;
	uint32_t t;

	for (p = 0; p < PAGES_MAX; p++)
		for (t = 0; t < UI_GRAPHPAGE_TRACES; t++)
			UiTrace_Clear(&History[p][t]);

	Step = 0u;
	DueUs = 0u;
}


/***************************************************************************************/
void UiGraphPage_Sample(uint32_t NowMs, uint32_t FrameUs)
{
	uint8_t p;

	if (DueUs > FrameUs)
	{
		DueUs -= FrameUs;
		return;
	}
	DueUs = UI_GRAPHPAGE_STEP_US;

	for (p = 0; p < PageCount && p < PAGES_MAX; p++)
	{
		const FaceElement_t *Elements[UI_GRAPHPAGE_TRACES];
		uint32_t n = UiGraphPage_Elements(p, Elements);
		uint32_t t;

		for (t = 0; t < n; t++)
		{
			UiWidget_t W = UiModel_Widget(Elements[t], NowMs);

			/* A reading that has never arrived, or is too old to trust, is a
			   gap in the trace - not a line held at its last value, which
			   would read as a measurement. */
			UiTrace_Push(&History[p][t],
			             (W.State == UI_STATE_NODATA || W.State == UI_STATE_STALE)
			             ? UI_GRAPH_GAP : W.Position);
		}
	}

	Step++;
}


/***************************************************************************************/
static void UiGraphPage_Plot(UiGraphPage_t *V)
{
	const UiTrace_t *Traces[UI_GRAPHPAGE_TRACES];
	UiGraphInk_t Ink;
	uint32_t t;

	for (t = 0; t < V->TraceCount; t++)
		Traces[t] = &History[V->Page][t];

	Ink.Background = UiDraw_Ink(UI_INK_FACE);
	Ink.Grid = UiDraw_Ink(UI_INK_GRID);
	Ink.Trace[0] = UiDraw_Ink(UI_INK_RED);
	Ink.Trace[1] = UiDraw_Ink(UI_INK_WHITE);

	UiGraph_Draw(UiDraw_Buffer(V->Surface), (uint32_t)UI_DRAW_WIDTH, &V->Interior,
	             Traces, V->TraceCount, &Ink, V->Rows, UI_GRAPHPAGE_COLS);
	V->DrawnStep = Step;
}


/***************************************************************************************/
/* The readings, over the plot: the first trace's on the left in its own red,
   the second's on the right in white. */
static void UiGraphPage_Readings(UiGraphPage_t *V, uint32_t NowMs, bool Force,
                                 UiGraphDirty_t Dirty, void *Ctx)
{
	const FaceElement_t *Elements[UI_GRAPHPAGE_TRACES];
	uint32_t n = UiGraphPage_Elements(V->Page, Elements);
	uint32_t t;

	for (t = 0; t < n; t++)
	{
		UiWidget_t W = UiModel_Widget(Elements[t], NowMs);
		int32_t Dx = (t == 0u) ? -UI_GRAPH_READING_DX : UI_GRAPH_READING_DX;
		int32_t Tx, Ty;
		UiRect_t Ink;

		if (!Force && V->HaveText[t] && strcmp(W.Text, V->Text[t]) == 0)
			continue;

		if (V->HaveText[t])
		{
			UiDraw_Restore(V->Surface, &V->TextRect[t]);
			if (Dirty != NULL)
				Dirty(Ctx, &V->TextRect[t]);
		}

		(void)snprintf(V->Text[t], sizeof(V->Text[t]), "%s", W.Text);
		UiText_Centre(&dash_font_value_56, V->Text[t],
		              (float)(V->Interior.X1 + V->Interior.X2) / 2.0f + (float)Dx,
		              (float)(V->Interior.Y1 + V->Interior.Y2) / 2.0f
		              + (float)UI_GRAPH_READING_DY, &Tx, &Ty);
		V->HaveText[t] = UiText_Bounds(&dash_font_value_56, V->Text[t], Tx, Ty, &Ink);
		if (V->HaveText[t])
		{
			(void)UiDraw_TextIn(V->Surface, (t == 0u) ? UI_TEXT_RED : UI_TEXT_WHITE,
			                    &dash_font_value_56, V->Text[t], Tx, Ty);
			V->TextRect[t] = Ink;
			if (Dirty != NULL)
				Dirty(Ctx, &Ink);
		}
	}
}


/***************************************************************************************/
void UiGraphPage_Load(UiGraphPage_t *V, uint8_t Surface, uint8_t Page,
                      float Cx, float Cy, uint32_t NowMs)
{
	const FaceElement_t *Elements[UI_GRAPHPAGE_TRACES];
	uint32_t Majors = 0;

	memset(V, 0, sizeof(*V));
	V->Surface = Surface;
	V->Page = Page;
	V->TraceCount = UiGraphPage_Elements(Page, Elements);

	/* The plot's interior, from the frame the face renderer drew. */
	V->Interior.X1 = (int32_t)Cx - (UI_GRAPH_FRAME_W / 2) + UI_GRAPH_FRAME_WIDTH;
	V->Interior.X2 = V->Interior.X1 + (int32_t)UI_GRAPH_COLUMNS - 1;
	V->Interior.Y1 = (int32_t)Cy - (UI_GRAPH_FRAME_H / 2) + UI_GRAPH_FRAME_WIDTH;
	V->Interior.Y2 = (int32_t)Cy + (UI_GRAPH_FRAME_H / 2) - UI_GRAPH_FRAME_WIDTH - 1;

	/* One grid band per interval between the first scale's labels, so the
	   lines land on the numbers printed beside them. */
	if (V->TraceCount != 0u && Elements[0]->Ticks != NULL)
		while (Elements[0]->Ticks[Majors] != NULL)
			Majors++;
	V->Rows = (Majors > 1u) ? (Majors - 1u) : 1u;

	UiGraphPage_Plot(V);
	UiGraphPage_Readings(V, NowMs, true, NULL, NULL);
}


/***************************************************************************************/
void UiGraphPage_Update(UiGraphPage_t *V, uint32_t NowMs, bool TextDue,
                        UiGraphDirty_t Dirty, void *Ctx)
{
	if (Step != V->DrawnStep)
	{
		UiGraphPage_Plot(V);
		Dirty(Ctx, &V->Interior);
	}

	if (TextDue)
		UiGraphPage_Readings(V, NowMs, false, Dirty, Ctx);
}
