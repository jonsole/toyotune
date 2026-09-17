/*
 * test_ui_graph.c - host tests for the strip chart.
 *
 * The history is a ring buffer read backwards from its newest sample, which is
 * the sort of arithmetic that is wrong by one and still looks plausible on the
 * glass; and a gap must stay a gap, since a trace held at its last value would
 * claim a reading the node never had.
 */

#include <stdio.h>
#include <string.h>

#include "pages.h"
#include "ui_graph.h"
#include "ui_model.h"

extern int GraphTests_Run(int *Checks, int *Failures);

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

/* A buffer wider than the plot, with a margin round it, so anything drawn
   outside the plot's interior is caught. */
#define W	(340)
#define H	(240)
#define PLOT_X	(20)
#define PLOT_Y	(25)

static uint8_t Buf[W * H];
static UiTrace_t TraceA;
static UiTrace_t TraceB;

static const UiGraphInk_t Ink = { 0x11u, 0x22u, { 0x33u, 0x44u } };


/***************************************************************************************/
static void TestHistory(void)
{
	UiTrace_t T;
	uint32_t i;

	UiTrace_Clear(&T);
	CHECK(UiTrace_Get(&T, 0) == UI_GRAPH_GAP, "an empty history is all gaps");

	UiTrace_Push(&T, 100u);
	UiTrace_Push(&T, 200u);
	UiTrace_Push(&T, 300u);
	CHECK(UiTrace_Get(&T, 0) == 300u, "the newest sample is age 0");
	CHECK(UiTrace_Get(&T, 1) == 200u, "then the one before it");
	CHECK(UiTrace_Get(&T, 2) == 100u, "and so on");
	CHECK(UiTrace_Get(&T, 3) == UI_GRAPH_GAP, "beyond what is held is a gap");

	/* Fill it past the end: the oldest fall off, and the newest is still
	   age 0 - the wrap is where an off-by-one would show. */
	UiTrace_Clear(&T);
	for (i = 0; i < UI_GRAPH_COLUMNS * 2u; i++)
		UiTrace_Push(&T, (uint16_t)(i % 1000u));
	CHECK(T.Count == UI_GRAPH_COLUMNS, "the history holds %u columns, has %u",
	      (unsigned)UI_GRAPH_COLUMNS, (unsigned)T.Count);
	for (i = 0; i < UI_GRAPH_COLUMNS; i++)
	{
		uint16_t Want = (uint16_t)(((UI_GRAPH_COLUMNS * 2u) - 1u - i) % 1000u);

		if (UiTrace_Get(&T, i) != Want)
		{
			CHECK(false, "age %u should be %u, got %u", (unsigned)i, Want,
			      UiTrace_Get(&T, i));
			break;
		}
	}
	CHECK(UiTrace_Get(&T, UI_GRAPH_COLUMNS) == UI_GRAPH_GAP,
	      "one past the end is a gap, not the newest again");

	/* A value beyond full scale is pinned, not wrapped - and a gap stays a
	   gap rather than being pinned to full scale. */
	UiTrace_Clear(&T);
	UiTrace_Push(&T, UI_POSITION_MAX + 500u);
	CHECK(UiTrace_Get(&T, 0) == UI_POSITION_MAX, "over-range pins to full scale");
	UiTrace_Push(&T, UI_GRAPH_GAP);
	CHECK(UiTrace_Get(&T, 0) == UI_GRAPH_GAP, "a gap survives being pushed");
}


/***************************************************************************************/
static void TestRows(void)
{
	UiRect_t In = { PLOT_X, PLOT_Y, PLOT_X + (int32_t)UI_GRAPH_COLUMNS - 1, PLOT_Y + 189 };

	CHECK(UiGraph_Row(&In, 0u) == In.Y2, "zero plots on the bottom row");
	CHECK(UiGraph_Row(&In, UI_POSITION_MAX) == In.Y1, "full scale on the top row");
	CHECK(UiGraph_Row(&In, UI_POSITION_MAX / 2u) == In.Y1 + 94
	      || UiGraph_Row(&In, UI_POSITION_MAX / 2u) == In.Y1 + 95,
	      "half scale in the middle, got row %d", UiGraph_Row(&In, UI_POSITION_MAX / 2u));
}


/***************************************************************************************/
/* The plot: background, grid, traces, and nothing outside the interior. */
static void TestDraw(void)
{
	UiRect_t In = { PLOT_X, PLOT_Y, PLOT_X + (int32_t)UI_GRAPH_COLUMNS - 1, PLOT_Y + 189 };
	const UiTrace_t *Traces[2] = { &TraceA, &TraceB };
	uint32_t Outside = 0, Grid = 0, A = 0, B = 0, Back = 0;
	int32_t x, y;
	uint32_t i;

	UiTrace_Clear(&TraceA);
	UiTrace_Clear(&TraceB);
	for (i = 0; i < UI_GRAPH_COLUMNS; i++)
	{
		/* A steadily climbing trace and a flat one. */
		UiTrace_Push(&TraceA, (uint16_t)((i * UI_POSITION_MAX) / UI_GRAPH_COLUMNS));
		UiTrace_Push(&TraceB, (uint16_t)(UI_POSITION_MAX / 4u));
	}

	memset(Buf, 0xEE, sizeof(Buf));
	UiGraph_Draw(Buf, W, &In, Traces, 2u, &Ink, 5u, 5u);

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++)
		{
			uint8_t v = Buf[(y * W) + x];

			if (x < In.X1 || x > In.X2 || y < In.Y1 || y > In.Y2)
			{
				if (v != 0xEEu)
					Outside++;
				continue;
			}
			if (v == Ink.Grid) Grid++;
			else if (v == Ink.Trace[0]) A++;
			else if (v == Ink.Trace[1]) B++;
			else if (v == Ink.Background) Back++;
		}

	CHECK(Outside == 0, "%u pixels drawn outside the plot", Outside);
	CHECK(Back > 0, "the plot has a background");
	CHECK(Grid > 0, "the plot has grid lines");

	/* The climbing trace: about one span per column, two pixels wide. */
	CHECK(A > UI_GRAPH_COLUMNS && A < UI_GRAPH_COLUMNS * 12u,
	      "the climbing trace covers %u pixels, expected a line's worth", A);

	/* The flat trace: exactly TRACE_PX rows over the whole width, less where
	   the other trace crosses it. */
	CHECK(B > (UI_GRAPH_COLUMNS * UI_GRAPH_TRACE_PX) - 40u
	      && B <= UI_GRAPH_COLUMNS * UI_GRAPH_TRACE_PX,
	      "the flat trace covers %u pixels, expected about %u", B,
	      (unsigned)(UI_GRAPH_COLUMNS * UI_GRAPH_TRACE_PX));

	/* The newest sample is at the right-hand edge: the climbing trace ends
	   near the top there, and started at the bottom on the left. */
	{
		int32_t RightTop = H, LeftTop = H;

		for (y = In.Y1; y <= In.Y2; y++)
		{
			if (Buf[(y * W) + In.X2] == Ink.Trace[0] && RightTop == H)
				RightTop = y;
			if (Buf[(y * W) + In.X1] == Ink.Trace[0] && LeftTop == H)
				LeftTop = y;
		}
		CHECK(RightTop < In.Y1 + 10, "the newest sample is at the right edge, row %d",
		      RightTop);
		CHECK(LeftTop > In.Y2 - 10, "the oldest is at the left edge, row %d", LeftTop);
	}

	/* A gap draws nothing: fill the history with gaps and the plot should
	   have no trace pixels at all. */
	UiTrace_Clear(&TraceA);
	UiTrace_Clear(&TraceB);
	for (i = 0; i < UI_GRAPH_COLUMNS; i++)
	{
		UiTrace_Push(&TraceA, UI_GRAPH_GAP);
		UiTrace_Push(&TraceB, UI_GRAPH_GAP);
	}
	memset(Buf, 0xEE, sizeof(Buf));
	UiGraph_Draw(Buf, W, &In, Traces, 2u, &Ink, 5u, 5u);
	A = 0;
	for (y = In.Y1; y <= In.Y2; y++)
		for (x = In.X1; x <= In.X2; x++)
			if (Buf[(y * W) + x] == Ink.Trace[0] || Buf[(y * W) + x] == Ink.Trace[1])
				A++;
	CHECK(A == 0, "a history of gaps draws no trace, got %u pixels", A);
}


/***************************************************************************************/
static void TestPageBound(void)
{
	CHECK(PageCount <= PAGES_MAX, "PAGES_MAX is %u but there are %u pages",
	      (unsigned)PAGES_MAX, (unsigned)PageCount);
}


/***************************************************************************************/
int GraphTests_Run(int *OutChecks, int *OutFailures)
{
	Checks = 0;
	Failures = 0;

	printf("graph - history, rows, plot drawing\n");
	TestHistory();
	TestRows();
	TestDraw();
	TestPageBound();

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
