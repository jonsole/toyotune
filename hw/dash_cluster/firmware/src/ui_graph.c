/*
 * ui_graph.c - the strip chart. See ui_graph.h.
 */

#include "ui_graph.h"

#include <string.h>

#include "ui_model.h"


/***************************************************************************************/
void UiTrace_Clear(UiTrace_t *T)
{
	T->Next = 0u;
	T->Count = 0u;
}

void UiTrace_Push(UiTrace_t *T, uint16_t Pos)
{
	if (Pos != UI_GRAPH_GAP && Pos > UI_POSITION_MAX)
		Pos = UI_POSITION_MAX;

	T->Pos[T->Next] = Pos;
	T->Next = (uint16_t)((T->Next + 1u) % UI_GRAPH_COLUMNS);
	if (T->Count < UI_GRAPH_COLUMNS)
		T->Count++;
}

uint16_t UiTrace_Get(const UiTrace_t *T, uint32_t Age)
{
	if (Age >= T->Count)
		return UI_GRAPH_GAP;
	return T->Pos[(T->Next + UI_GRAPH_COLUMNS - 1u - Age) % UI_GRAPH_COLUMNS];
}


/***************************************************************************************/
int32_t UiGraph_Row(const UiRect_t *In, uint16_t Pos)
{
	int32_t Height = In->Y2 - In->Y1;

	return In->Y2 - (int32_t)(((int32_t)Pos * Height + (UI_POSITION_MAX / 2)) / UI_POSITION_MAX);
}


/***************************************************************************************/
static void UiGraph_Span(uint8_t *Buffer, uint32_t Stride, const UiRect_t *In, int32_t X,
                         int32_t YA, int32_t YB, uint8_t Ink)
{
	int32_t Top = (YA < YB) ? YA : YB;
	int32_t Bottom = (YA < YB) ? YB : YA;
	int32_t Y;

	/* Thickness is added downward and across, so a flat trace is exactly
	   TRACE_PX rows and a column is never shared with the next one's span. */
	Bottom += UI_GRAPH_TRACE_PX - 1;
	if (Top < In->Y1)
		Top = In->Y1;
	if (Bottom > In->Y2)
		Bottom = In->Y2;

	for (Y = Top; Y <= Bottom; Y++)
	{
		uint8_t *Row = Buffer + ((uint32_t)Y * Stride);
		int32_t K;

		for (K = 0; K < UI_GRAPH_TRACE_PX; K++)
			if (X + K <= In->X2)
				Row[X + K] = Ink;
	}
}


/***************************************************************************************/
void UiGraph_Draw(uint8_t *Buffer, uint32_t Stride, const UiRect_t *In,
                  const UiTrace_t *const *Traces, uint32_t TraceCount,
                  const UiGraphInk_t *Ink, uint32_t GridRows, uint32_t GridCols)
{
	int32_t Width = In->X2 - In->X1 + 1;
	int32_t Height = In->Y2 - In->Y1 + 1;
	int32_t X, Y;
	uint32_t k, t;

	/* Background. */
	for (Y = In->Y1; Y <= In->Y2; Y++)
		memset(Buffer + ((uint32_t)Y * Stride) + (uint32_t)In->X1, Ink->Background,
		       (size_t)Width);

	/* The inner grid lines; the frame round the plot is the face's. */
	for (k = 1; k < GridRows; k++)
	{
		Y = In->Y1 + (int32_t)((k * (uint32_t)(Height - 1) + GridRows / 2u) / GridRows);
		memset(Buffer + ((uint32_t)Y * Stride) + (uint32_t)In->X1, Ink->Grid, (size_t)Width);
	}
	for (k = 1; k < GridCols; k++)
	{
		X = In->X1 + (int32_t)((k * (uint32_t)Width) / GridCols);
		for (Y = In->Y1; Y <= In->Y2; Y++)
			Buffer[((uint32_t)Y * Stride) + (uint32_t)X] = Ink->Grid;
	}

	/* The traces, last first so the first is on top. Column X shows the
	   sample (Width - 1 - offset) columns old, joined to the one before it
	   by a vertical span - which is what makes a steep change a line rather
	   than two separate dots. */
	if (TraceCount > 2u)
		TraceCount = 2u;
	for (t = TraceCount; t-- > 0u;)
	{
		for (X = In->X1; X <= In->X2; X++)
		{
			uint32_t Age = (uint32_t)(In->X2 - X);
			uint16_t Pos = UiTrace_Get(Traces[t], Age);
			uint16_t Prev = UiTrace_Get(Traces[t], Age + 1u);
			int32_t Row;

			if (Pos == UI_GRAPH_GAP)
				continue;
			Row = UiGraph_Row(In, Pos);
			UiGraph_Span(Buffer, Stride, In, X, Row,
			             (Prev == UI_GRAPH_GAP) ? Row : UiGraph_Row(In, Prev),
			             Ink->Trace[t]);
		}
	}
}
