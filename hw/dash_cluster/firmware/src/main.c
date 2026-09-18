/*
 * main.c
 *
 * Dash node entry point and the two-core split.
 *
 *   Core 0   can2040 and its PIO interrupt, frame decode, the signal store
 *   Core 1   the panel and the renderer
 *
 * That split is not arbitrary. can2040 is software CAN: it decodes the bus a
 * bit at a time in an interrupt, so it is sensitive to interrupt latency in a
 * way rendering is not. Keeping the renderer on the other core means a long
 * draw cannot delay a CAN bit, and its documentation is explicit that the
 * can2040 code should live in SRAM rather than XIP flash for the same reason -
 * a cache miss inside the CAN interrupt corrupts a bit.
 *
 * The remaining risk is bus contention rather than CPU time: the panel driver
 * moves colour data by DMA in large bursts, competing with the core servicing
 * the CAN interrupt. PLAN.md section 4.2a lists the levers in the order to
 * try them, and M4 is the gate that decides whether can2040 survives at all
 * or the design falls back to an MCP2518FD.
 *
 * The panel is panel.c and the renderer is ui_draw.c. With DASH_HAVE_PANEL off
 * core 1 reports what it would have drawn over USB serial instead - which is
 * enough to exercise the store, the page tables and the cross-core handover
 * with no glass attached.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"

#include "node_id.h"
#include "pages.h"
#include "signal_store.h"
#include "signals.h"
#include "telemetry.h"

#if DASH_HAVE_CAN2040
#include "can_link.h"
#endif

#if DASH_HAVE_PANEL
#include "dash_faces.h"
#include "imu.h"
#include "panel.h"
#include "audio.h"
#include "clock_link.h"
#include "rtc.h"
#include "ui_draw.h"
#include "ui_gauge.h"
#include "ui_clockpage.h"
#include "ui_gpage.h"
#include "warn.h"
#include "ui_graphpage.h"
#include "ui_model.h"
#include "ui_needle.h"
#include "ui_text.h"

extern const DashFont_t dash_font_value_56;

/* How often the reading in the middle of the dial may change, in milliseconds.
   0 redraws it on every frame the value changes - the heaviest case, and the
   one to measure before choosing a calmer rate for the car: a digital readout
   that changes 60 times a second is smooth but not readable, which is why
   instruments usually settle it to a few updates a second. */
#define CORE1_VALUE_PERIOD_MS	(0u)
#endif

/* How often the console status line goes out. Long, because it is a
   background reassurance rather than a data feed - the glass is the data
   feed. */
#define STATUS_PERIOD_MS	(2000u)

#if DASH_SIMULATE
/* SIMULATED TELEMETRY - a bench aid, never a car build.
 *
 * With no Toyotune board on the bus every gauge reads "--", which leaves the
 * value readout, the needle's response to real numbers and the warning bands
 * unexercised. This writes a plausible rev-and-boost cycle into the signal
 * store at the FAST tier's own 20 ms period, through SignalStore_Set() - the
 * same door telemetry.c uses - so everything downstream of the store runs
 * exactly as it would on real data, staleness included.
 *
 * Two things follow from going through the real door, and both are why this
 * is a build option that defaults off. The store cannot tell these values from
 * real ones, so it reports the link alive; the status line and the boot banner
 * say SIMULATED instead, so a simulated node cannot pass for a connected one.
 * And real telemetry arriving on the bus would be overwritten every 20 ms, so
 * a simulate build must not be put on a bus with a Toyotune board on it. */
#define SIM_PERIOD_MS		(20u)	/* the FAST tier, which RPM and MAP ride */
#define SIM_CYCLE_MS		(12000u)	/* idle to near the limiter and back */
#define SIM_RPM_IDLE		(800)
#define SIM_RPM_TOP		(7000)

static void Simulate(uint32_t NowMs)
{
	/* A triangle: up for half the cycle, down for the other half. */
	int32_t Half = (int32_t)(SIM_CYCLE_MS / 2u);
	int32_t Phase = (int32_t)(NowMs % SIM_CYCLE_MS);
	int32_t T = (Phase < Half) ? Phase : ((int32_t)SIM_CYCLE_MS - Phase);
	int32_t Rpm = SIM_RPM_IDLE + (T * (SIM_RPM_TOP - SIM_RPM_IDLE)) / Half;
	int32_t Map;

	/* Manifold pressure in tenths of a kPa, following the revs the way a
	   turbo engine does: vacuum at idle, atmospheric by 3000, then boost
	   building to 230 kPa at the top - inside the gauge's 250 kPa face. */
	if (Rpm <= 3000)
		Map = 350 + ((Rpm - SIM_RPM_IDLE) * (1000 - 350)) / (3000 - SIM_RPM_IDLE);
	else
		Map = 1000 + ((Rpm - 3000) * (2300 - 1000)) / (SIM_RPM_TOP - 3000);

	SignalStore_Set(SIGNAL_RPM, Rpm, NowMs);
	SignalStore_Set(SIGNAL_MAP, Map, NowMs);

	/* Mixture, in hundredths of AFR: near stoichiometric off boost, richening
	   to 11.5 as boost builds on the way up, and lean on the overrun as the
	   revs fall - the shapes a real wideband shows. */
	{
		int32_t Afr;

		if (Phase < Half && Map > 1013)
			Afr = 1470 - ((Map - 1013) * (1470 - 1150)) / (2300 - 1013);
		else if (Phase >= Half && Rpm > 1500)
			Afr = 1650 + (T * 250) / Half;
		else
			Afr = 1470;
		SignalStore_Set(SIGNAL_AFR, Afr, NowMs);
	}
}
#endif

/* There is no UI update period any more: the gauges are updated once per
   panel frame, locked to its TE pulse. See Core1Main(). */

/* How often core 0 says where core 1 got to, while the panel is still coming
   up. Frequent, because this only runs when something is wrong. */
#define STAGE_REPORT_PERIOD_MS	(500u)

/* Latched in main() and read by core 1.
 *
 * USB CDC discards everything printed before a host attaches, so the boot
 * banner is invisible to anyone who connects afterwards - which, on a board
 * that powers up with the ignition, is everyone. Repeating the identity in the
 * periodic status line is what makes it observable at all. */
static uint8_t DashNodeId;

/* A screenshot asked for over the console - an 'S' - raised by core 0, which
   owns stdin, and served by core 1 at the end of a frame. */
static volatile bool ScreenshotRequested;

/* A page change asked for over the console - 'n' next, 'p' previous - served
   by core 1, which owns the page selection. +1, -1, or 0 for none. */
static volatile int8_t PageStepRequested;

/* A clock setting asked for over the console - "Thhmmss" - served by core 1,
   which owns the I2C bus the clock is on. Written by core 0 and read by
   core 1: the flag is set last and cleared first, so core 1 can never act on
   a half-written time. */
static volatile uint8_t TimeRequest[3];
static volatile bool TimeRequested;

/* A sound asked for from the console, so the speaker can be proved on a bench
   with no engine turning. 0xFF is "nothing asked for"; anything else overrides
   the real warning for BEEP_TEST_MS. */
static volatile uint8_t BeepRequest = 0xFFu;
static volatile uint8_t VolumeRequest = 0xFFu;
#define BEEP_TEST_MS		(3000u)

#if DASH_HAVE_PANEL
/* CORE 1'S STACK, AND WHY IT IS NOT THE SDK'S.
 *
 * The SDK reserves core 1's stack in SCRATCH_X, which on RP2350 is a single
 * 4 KB bank - and its default is 2 KB of that. The vendor panel driver builds
 * a per-row fill buffer on the stack, and a stack that overruns there does not
 * report itself: it appears as a hard fault somewhere inside a draw, which is
 * a thoroughly miserable thing to chase on a bench.
 *
 * 4 KB - all of SCRATCH_X - would still be tight, so the stack goes in main
 * SRAM instead and is sized generously. The cost is that core 1's stack
 * accesses now contend with core 0 and with the panel DMA rather than sitting
 * in their own bank. That is the same contention M4 measures, and if it bites,
 * bank placement is the first lever PLAN.md section 4.2a reaches for. */
#define CORE1_STACK_BYTES	(8u * 1024u)
static uint32_t Core1Stack[CORE1_STACK_BYTES / sizeof(uint32_t)];
#endif


/***************************************************************************************/
/* The console report of one face, used when there is no panel. Kept because it
   is how the store, the page tables and the cross-core handover were proven
   before any glass was attached, and it is still the way to tell whether a
   blank screen means no data or no display. */
static void Core1ReportFace(uint32_t NowMs, uint8_t Page, bool Verbose)
{
	const FacePage_t *Face = &Pages[Page];
	char Text[24];
	uint8_t i;

	if (!SignalStore_LinkAlive(NowMs))
	{
		uint32_t AgeMs = SignalStore_LinkAgeMs(NowMs);

		/* UINT32_MAX is the store's "nothing has ever arrived", not an age.
		   Printing it as one gives "no telemetry for 4294967295ms", a number
		   that looks like data and is not - which is the exact failure this
		   firmware is careful about everywhere else. */
		if (AgeMs == UINT32_MAX)
			printf("  node %u: no telemetry, none ever received\n", DashNodeId);
		else
			printf("  node %u: no telemetry for %lums\n",
			       DashNodeId, (unsigned long)AgeMs);
		return;
	}

	if (Telemetry_ProtocolMismatch())
	{
		/* Deliberately loud, and deliberately not a gauge: the layout may
		   have moved, so any value drawn from it could be wrong. */
		printf("  PROTOCOL v%u, expected v%u - refusing to decode\n",
		       Telemetry_SeenProtocolVersion(), DASH_EXPECTED_PROTOCOL_VERSION);
		return;
	}

	if (!Verbose)
		return;

	for (i = 0; i < Face->ElementCount; i++)
	{
		const FaceElement_t *E = &Face->Elements[i];
		SignalReading_t R = SignalStore_Get(E->Signal, NowMs);
		const SignalDescriptor_t *D = &SignalDescriptors[E->Signal];

		if (!R.Valid)
			printf("  %-9s --\n", D->Name);
		else
			printf("  %-9s %s %s%s\n", D->Name,
			       Signal_Format(E->Signal, R.Value, Text, sizeof(Text)),
			       D->Unit, R.Fresh ? "" : "  (STALE)");
	}
}


#if DASH_HAVE_PANEL
/***************************************************************************************/
/* A page's live state. A page has one face and up to CORE1_MAX_GAUGES gauges
   on it - one full dial, or a top and a bottom half - each with its own needle
   and reading. It lives in one of ui_draw's surfaces. */
#define CORE1_MAX_GAUGES	(2u)

typedef struct
{
	const FaceElement_t *Element;
	int32_t X, Y, W, H;		/* the element, in panel pixels */

	uint32_t SmoothQ;		/* the eased needle position */
	bool HaveNeedle;
	UiNeedle_t Needle;
	UiRect_t NeedleRect;

	bool HaveText;
	char Text[sizeof(((UiWidget_t *)0)->Text)];
	UiRect_t TextRect;
} Core1Gauge_t;

typedef struct
{
	uint8_t Page;
	uint8_t Surface;
	bool Valid;			/* a face was found and fits */
	const DashFace_t *Face;
	uint8_t GaugeCount;
	Core1Gauge_t Gauges[CORE1_MAX_GAUGES];

	/* A g-force page instead of, or as well as, gauges. */
	bool HasG;
	UiGPage_t G;

	/* Or a strip chart. */
	bool HasGraph;
	UiGraphPage_t Graph;

	/* Or the clock. */
	bool HasClock;
	UiClockPage_t Clock;
} Core1Page_t;


/***************************************************************************************/
/* The rectangles a frame has changed, kept apart where they are apart - two
   needles in opposite halves of a face - and merged where they touch. */
typedef struct
{
	uint32_t Count;
	PanelRect_t Rects[PANEL_MAX_REGIONS];
} Core1Dirty_t;

static bool Core1Touches(const PanelRect_t *A, const UiRect_t *B)
{
	return A->X1 <= B->X2 + 1 && B->X1 <= A->X2 + 1
	       && A->Y1 <= B->Y2 + 1 && B->Y1 <= A->Y2 + 1;
}

static void Core1Dirty(Core1Dirty_t *D, const UiRect_t *R)
{
	UiRect_t M = *R;
	uint32_t i = 0;

	/* Merge with everything it touches, repeatedly, since a merged rectangle
	   can reach one it did not touch before. */
	while (i < D->Count)
	{
		if (Core1Touches(&D->Rects[i], &M))
		{
			PanelRect_t *P = &D->Rects[i];

			M.X1 = (P->X1 < M.X1) ? P->X1 : M.X1;
			M.Y1 = (P->Y1 < M.Y1) ? P->Y1 : M.Y1;
			M.X2 = (P->X2 > M.X2) ? P->X2 : M.X2;
			M.Y2 = (P->Y2 > M.Y2) ? P->Y2 : M.Y2;
			D->Rects[i] = D->Rects[--D->Count];
			i = 0;
		}
		else
		{
			i++;
		}
	}

	if (D->Count == PANEL_MAX_REGIONS)
	{
		/* Out of room: fold it into the last one. Correct, just larger. */
		PanelRect_t *P = &D->Rects[D->Count - 1u];

		P->X1 = (P->X1 < M.X1) ? P->X1 : M.X1;
		P->Y1 = (P->Y1 < M.Y1) ? P->Y1 : M.Y1;
		P->X2 = (P->X2 > M.X2) ? P->X2 : M.X2;
		P->Y2 = (P->Y2 > M.Y2) ? P->Y2 : M.Y2;
		return;
	}

	D->Rects[D->Count].X1 = M.X1;
	D->Rects[D->Count].Y1 = M.Y1;
	D->Rects[D->Count].X2 = M.X2;
	D->Rects[D->Count].Y2 = M.Y2;
	D->Count++;
}


/***************************************************************************************/
/* A gauge's needle at an eased position. The pivot is the element's true
   centre - half-pixel and all, see ui_needle.h - which is where the face
   renderer put the centre of the dial. */
static UiNeedle_t Core1Needle(const Core1Gauge_t *G, uint32_t PositionQ)
{
	return UiNeedle_Place((float)G->X + ((float)G->W / 2.0f),
	                      (float)G->Y + ((float)G->H / 2.0f),
	                      (float)UiGauge_NeedleInner(G->W, G->H),
	                      (float)UiGauge_NeedleOuter(G->W, G->H),
	                      (float)UI_GAUGE_NEEDLE_WIDTH / 2.0f,
	                      UiGauge_SweepStart(G->Element->Sweep),
	                      UiGauge_SweepSpan(G->Element->Sweep),
	                      PositionQ);
}


/***************************************************************************************/
/* Frame counters for the status line. */
typedef struct
{
	uint32_t Needles;
	uint32_t Values;
	uint32_t Still;
	uint32_t Slides;
	uint32_t Swipes;
	uint32_t Cancels;
	uint32_t Taps;
	uint32_t Zeroed;
	uint32_t ZeroRefused;
} Core1Counts_t;


/***************************************************************************************/
/* One gauge's frame: ease its needle and refresh its reading, restoring the
   face under whatever moved and noting what changed.

   NOTHING ON A FACE OVERLAPS ANYTHING ELSE LIVE: readings sit inside the centre
   ring, needles start outside it, and a split face's two needles keep to their
   own halves. So each item can be restored and redrawn without disturbing the
   others. */
static void Core1UpdateGauge(uint8_t Surface, Core1Gauge_t *G, uint32_t NowMs,
                             uint32_t FrameUs, bool TextDue, Core1Dirty_t *Dirty,
                             Core1Counts_t *Counts)
{
	UiWidget_t W = UiModel_Widget(G->Element, NowMs);
	UiNeedle_t Next;

	G->SmoothQ = UiModel_NeedleStep(G->SmoothQ, W.Position, FrameUs);
	Next = Core1Needle(G, G->SmoothQ);

	if (TextDue && strcmp(W.Text, G->Text) != 0)
	{
		int32_t Tx, Ty;
		UiRect_t Ink;

		if (G->HaveText)
		{
			UiDraw_Restore(Surface, &G->TextRect);
			Core1Dirty(Dirty, &G->TextRect);
		}

		(void)snprintf(G->Text, sizeof(G->Text), "%s", W.Text);
		UiText_Centre(&dash_font_value_56, G->Text,
		              (float)G->X + ((float)G->W / 2.0f),
		              (float)G->Y + ((float)G->H / 2.0f)
		              + (float)UiGauge_ReadingDy(G->Element->Sweep),
		              &Tx, &Ty);
		G->HaveText = UiText_Bounds(&dash_font_value_56, G->Text, Tx, Ty, &Ink);
		if (G->HaveText)
		{
			(void)UiDraw_Text(Surface, &dash_font_value_56, G->Text, Tx, Ty);
			G->TextRect = Ink;
			Core1Dirty(Dirty, &Ink);
		}
		Counts->Values++;
	}

	if (!G->HaveNeedle || !UiNeedle_Same(&Next, &G->Needle))
	{
		UiRect_t NextRect = UiNeedle_Bounds(&Next);

		if (G->HaveNeedle)
		{
			UiDraw_Restore(Surface, &G->NeedleRect);
			Core1Dirty(Dirty, &G->NeedleRect);
		}
		(void)UiDraw_Needle(Surface, &Next);
		Core1Dirty(Dirty, &NextRect);

		G->Needle = Next;
		G->NeedleRect = NextRect;
		G->HaveNeedle = true;
		Counts->Needles++;
	}
}

static void Core1DirtySink(void *Context, const UiRect_t *R)
{
	Core1Dirty((Core1Dirty_t *)Context, R);
}

static void Core1UpdatePage(Core1Page_t *V, uint32_t NowMs, uint32_t FrameUs,
                            bool TextDue, Core1Dirty_t *Dirty, Core1Counts_t *Counts)
{
	uint8_t g;

	for (g = 0; V->Valid && g < V->GaugeCount; g++)
		Core1UpdateGauge(V->Surface, &V->Gauges[g], NowMs, FrameUs, TextDue, Dirty,
		                 Counts);

	if (V->Valid && V->HasG)
		UiGPage_Update(&V->G, TextDue, Core1DirtySink, Dirty);

	if (V->Valid && V->HasGraph)
		UiGraphPage_Update(&V->Graph, NowMs, TextDue, Core1DirtySink, Dirty);

	if (V->Valid && V->HasClock)
		UiClockPage_Update(&V->Clock, TextDue, Core1DirtySink, Dirty);
}


/***************************************************************************************/
/* Build a page from scratch into a surface: its face, and its needles and
   readings on their current values - not swinging up from zero. */
static void Core1LoadPage(Core1Page_t *V, uint8_t Page, uint8_t Surface, uint32_t NowMs)
{
	Core1Dirty_t Ignored = { 0u, { { 0, 0, 0, 0 } } };
	Core1Counts_t Unused;
	const FaceElement_t *GElement = NULL;
	int32_t FaceX, FaceY;
	uint8_t i, e;

	memset(V, 0, sizeof(*V));
	V->Page = Page;
	V->Surface = Surface;
	UiDraw_Clear(Surface);

	for (i = 0; i < DashFaceCount; i++)
		if (DashFaces[i].Page == Page)
			V->Face = &DashFaces[i];

	for (e = 0; e < Pages[Page].ElementCount && V->GaugeCount < CORE1_MAX_GAUGES; e++)
	{
		const FaceElement_t *El = &Pages[Page].Elements[e];
		Core1Gauge_t *G;

		if (El->Type == WIDGET_GFORCE && !V->HasG)
		{
			V->HasG = true;
			GElement = El;
			continue;
		}
		if (El->Type == WIDGET_GRAPH)
		{
			if (!V->HasGraph)
			{
				V->HasGraph = true;
				GElement = El;
			}
			continue;
		}
		if (El->Type == WIDGET_CLOCK && !V->HasClock)
		{
			V->HasClock = true;
			GElement = El;
			continue;
		}
		if (El->Type != WIDGET_GAUGE)
			continue;

		G = &V->Gauges[V->GaugeCount++];
		G->Element = El;
		G->X = UiGauge_Pct(El->X, PANEL_WIDTH);
		G->Y = UiGauge_Pct(El->Y, PANEL_HEIGHT);
		G->W = UiGauge_Pct(El->W, PANEL_WIDTH);
		G->H = UiGauge_Pct(El->H, PANEL_HEIGHT);
		G->SmoothQ = (uint32_t)UiModel_Widget(El, NowMs).Position << UI_NEEDLE_Q;
	}

	if (V->Face == NULL
	    || (V->GaugeCount == 0u && !V->HasG && !V->HasGraph && !V->HasClock))
	{
		printf("page %u: no pre-rendered face - black\n", Page);
		return;
	}

	/* The face sits where its first element does - they share a rectangle. */
	if (V->GaugeCount != 0u)
	{
		FaceX = V->Gauges[0].X;
		FaceY = V->Gauges[0].Y;
	}
	else
	{
		FaceX = UiGauge_Pct(GElement->X, PANEL_WIDTH);
		FaceY = UiGauge_Pct(GElement->Y, PANEL_HEIGHT);
	}

	if (!UiDraw_LoadFace(Surface, V->Face, FaceX, FaceY))
	{
		printf("page %u: %ldx%ld face does not fit at %ld,%ld - black\n",
		       Page, (long)V->Face->Width, (long)V->Face->Height,
		       (long)FaceX, (long)FaceY);
		return;
	}

	/* The g view first: the update below draws it, and before it has been
	   given its centre it would draw round the corner of the screen - which
	   it did, leaving a stray reading and marks at the top left that nothing
	   ever cleaned up. */
	if (V->HasG)
		UiGPage_Load(&V->G, Surface,
		             (float)FaceX + (float)V->Face->Width / 2.0f,
		             (float)FaceY + (float)V->Face->Height / 2.0f);

	if (V->HasGraph)
		UiGraphPage_Load(&V->Graph, Surface, Page,
		                 (float)FaceX + (float)V->Face->Width / 2.0f,
		                 (float)FaceY + (float)V->Face->Height / 2.0f, NowMs);

	if (V->HasClock)
		UiClockPage_Load(&V->Clock, Surface,
		                 (float)FaceX + (float)V->Face->Width / 2.0f,
		                 (float)FaceY + (float)V->Face->Height / 2.0f,
		                 UiGauge_Radius(V->Face->Width, V->Face->Height));

	V->Valid = true;
	memset(&Unused, 0, sizeof(Unused));
	Core1UpdatePage(V, NowMs, 0u, true, &Ignored, &Unused);
}


/***************************************************************************************/
/* THE SWIPE.
 *
 * The page follows the finger: once a touch has moved CORE1_DRAG_START_PX
 * across, and twice as far across as up or down, the neighbouring page is
 * built into the other surface and the two are shown side by side at the
 * finger's offset - both live, needles and readings still moving. Finger left
 * brings in the next page from the right, as on a phone.
 *
 * WITH MOMENTUM. At lift-off the finger's speed is measured over its last
 * CORE1_SPEED_WINDOW_US, and the page is where it would be CORE1_PROJECT_US
 * later at that speed: past halfway it completes, short of it it springs back,
 * and a hard flick decides on its own. Then it carries on at the finger's
 * speed and slows at a constant rate to stop exactly on the page - so it never
 * stops dead at release and restarts, which is what reads as a stall. A slow
 * release is given a minimum speed for the same reason, and a violent flick a
 * maximum, so the settle always takes between CORE1_SETTLE_MIN_US and
 * CORE1_SETTLE_MAX_US.
 *
 * A completed swipe just swaps the two surfaces' roles - the incoming page is
 * already drawn - and selects the page.
 *
 * A fault takeover outranks all of it: no swipe starts while one stands, and
 * one arriving mid-swipe ends the swipe where it is. */
#define CORE1_DRAG_START_PX	(16)
#define CORE1_TAP_WANDER_PX	(12)
#define CORE1_TAP_MAX_US	(350000u)
#define CORE1_LONG_PRESS_US	(1500000u)
#define CORE1_FLICK_PX_PER_S	(600)		/* lift-off speed that decides on its own */
#define CORE1_PROJECT_US	(200000)	/* how far ahead momentum is projected */
#define CORE1_SPEED_WINDOW_US	(100000u)	/* the finger's speed, over this much of its path */
#define CORE1_SETTLE_MIN_US	(80000)
#define CORE1_SETTLE_MAX_US	(250000)
#define CORE1_SPEED_SAMPLES	(8u)

typedef enum
{
	SWIPE_IDLE,
	SWIPE_DRAG,
	SWIPE_SETTLE
} Core1SwipeState_t;

typedef struct
{
	Core1SwipeState_t State;
	int Direction;			/* +1 next page from the right, -1 previous from the left */
	int32_t Revealed;		/* pixels of the incoming page on the glass */
	int32_t Target;			/* SETTLE: PANEL_WIDTH to complete, 0 to cancel */

	bool Down;
	int32_t X0, Y0, X, Y;

	/* For taps and long presses: when the finger landed, how far it has
	   wandered, and whether this press has already done its long-press. */
	uint32_t DownUs;
	int32_t Wander;
	bool Dragged;
	bool LongDone;

	/* The finger's recent path, one sample per touch report, newest last. */
	uint32_t Samples;
	uint32_t LastReports;
	int32_t SampleX[CORE1_SPEED_SAMPLES];
	uint32_t SampleUs[CORE1_SPEED_SAMPLES];

	/* SETTLE: from where, at what speed towards the target (px/s), and for how
	   long, starting when. */
	int32_t From;
	int32_t Speed;
	int32_t DurationUs;
	uint32_t StartUs;
} Core1Swipe_t;

static void Core1SwipeSample(Core1Swipe_t *S, int32_t X, uint32_t NowUs)
{
	if (S->Samples == CORE1_SPEED_SAMPLES)
	{
		memmove(S->SampleX, S->SampleX + 1, sizeof(S->SampleX[0]) * (CORE1_SPEED_SAMPLES - 1u));
		memmove(S->SampleUs, S->SampleUs + 1, sizeof(S->SampleUs[0]) * (CORE1_SPEED_SAMPLES - 1u));
		S->Samples--;
	}
	S->SampleX[S->Samples] = X;
	S->SampleUs[S->Samples] = NowUs;
	S->Samples++;
}

/* The finger's speed at lift-off, px/s, over its last CORE1_SPEED_WINDOW_US of
   reports. Measured back from the LAST REPORT rather than from now, so the
   time it took to notice the lift does not dilute it; and a finger that
   stopped before lifting still reports, so its speed comes out as zero. */
static int32_t Core1SwipeSpeed(const Core1Swipe_t *S)
{
	uint32_t Last, First;

	if (S->Samples < 2u)
		return 0;

	Last = S->Samples - 1u;
	First = Last;
	while (First > 0u && (S->SampleUs[Last] - S->SampleUs[First - 1u]) <= CORE1_SPEED_WINDOW_US)
		First--;
	if (First == Last)
		return 0;

	return (int32_t)(((int64_t)(S->SampleX[Last] - S->SampleX[First]) * 1000000)
	                 / (int64_t)(S->SampleUs[Last] - S->SampleUs[First]));
}

/* The screen during a swipe: two surfaces side by side. Columns before
   PANEL_WIDTH - Offset come from Left, shifted left by Offset; the rest from
   the start of Right. */
typedef struct
{
	const uint8_t *Left;
	const uint8_t *Right;
	int32_t Offset;
} Core1Slide_t;

static uint32_t Core1SlideRow(void *Context, int32_t Y, int32_t X1, uint32_t Width,
                              PanelSpan_t *Spans)
{
	const Core1Slide_t *S = (const Core1Slide_t *)Context;
	uint32_t Row = (uint32_t)Y * (uint32_t)PANEL_WIDTH;
	uint32_t LeftCount = (uint32_t)(PANEL_WIDTH - S->Offset);

	/* Only ever asked for whole rows. */
	(void)X1;
	(void)Width;

	Spans[0].Src = S->Left + Row + (uint32_t)S->Offset;
	Spans[0].Count = LeftCount;
	Spans[1].Src = S->Right + Row;
	Spans[1].Count = (uint32_t)S->Offset;
	return 2u;
}


/***************************************************************************************/
/* THE SCREENSHOT. What the panel was last sent from the page on the glass, as
 * text on the console: the palette, then every row as hex indices, framed so
 * tools/screenshot.py can find it among the status lines and turn it into a
 * PNG. About 440 KB of text; core 1 misses a few frames while it goes out,
 * which is a fair price for being able to see the glass without a camera.
 */
static void Core1Screenshot(uint8_t Surface)
{
	static const char Hex[] = "0123456789abcdef";
	const uint8_t *Px = UiDraw_Buffer(Surface);
	const uint16_t *Pal = UiDraw_Palette();
	char Line[(PANEL_WIDTH * 2) + 2];
	uint32_t x, y;

	printf("\nSCREENSHOT BEGIN %d %d\n", PANEL_WIDTH, PANEL_HEIGHT);

	/* Palette entries as the panel receives them: high byte first. */
	printf("PALETTE ");
	for (x = 0; x < UI_DRAW_PALETTE_MAX; x++)
	{
		uint8_t Hi = (uint8_t)(Pal[x] & 0xFFu);
		uint8_t Lo = (uint8_t)(Pal[x] >> 8);

		printf("%02x%02x", Hi, Lo);
	}
	printf("\n");

	for (y = 0; y < (uint32_t)PANEL_HEIGHT; y++)
	{
		const uint8_t *Row = Px + (y * (uint32_t)PANEL_WIDTH);

		for (x = 0; x < (uint32_t)PANEL_WIDTH; x++)
		{
			Line[2 * x] = Hex[Row[x] >> 4];
			Line[(2 * x) + 1] = Hex[Row[x] & 0x0Fu];
		}
		Line[2 * PANEL_WIDTH] = '\0';

		/* Numbered, so a row lost on the way can be named rather than
		   shifting every row after it; flushed, so the USB side is not
		   outrun; and core 1 reports itself alive, or core 0's stall
		   watcher prints into the middle of the dump. */
		printf("R %03lu %s\n", (unsigned long)y, Line);
		stdio_flush();
		Panel_Alive(PANEL_STAGE_DRAW);
	}

	printf("SCREENSHOT END\n");
}


/***************************************************************************************/
/* Core 1: the display, one panel frame per loop.
 *
 * The loop is clocked by the panel: every push waits for the TE pulse, and a
 * frame with nothing to send waits for it instead - so the loop runs at the
 * scan rate with no timer in it anywhere.
 *
 * Normally each frame updates the page's gauges and sends the rectangles that
 * changed. During a swipe it updates both pages and sends the whole screen,
 * composed from the two surfaces as it goes out.
 *
 * Status goes out every STATUS_PERIOD_MS from here too. Printing it costs a
 * frame, once every two seconds. */
static void Core1Main(void)
{
	static Core1Page_t Views[UI_DRAW_SURFACES];
	Core1Page_t *Cur = &Views[0];
	Core1Page_t *In = &Views[1];
	Core1Swipe_t Swipe;
	Core1Counts_t Counts;
	Warn_t Warn;
	ToneId_t BeepId = TONE_NONE;
	uint32_t BeepUntilMs = 0u;
	uint32_t NextStatusMs = 0;
	uint32_t LastFrameUs;
	uint32_t NextValueMs = 0;
	uint32_t DrawUs = 0, PushPixels = 0, WorkUs = 0, WorkMaxUs = 0;
	uint32_t SlideWorkMaxUs = 0;
	bool Started = false;

	memset(&Swipe, 0, sizeof(Swipe));
	memset(&Counts, 0, sizeof(Counts));
	Views[0].Surface = 0u;
	Views[1].Surface = 1u;

	Panel_Init();
	(void)Imu_Init();
	(void)Rtc_Init();

	/* After Panel_Init, which brings up the I2C bus the codec shares with the
	   touch panel, the IMU and the clock. On core 1 because that bus has one
	   owner, and because the refill interrupt is better anywhere but the core
	   can2040 is on - see audio.h. */
	(void)Audio_Init();
	Warn_Init(&Warn);
	ClockLink_Init();
	UiClockPage_Init();
	UiGPage_Init();
	UiGraphPage_Init();
	UiDraw_Init();
	LastFrameUs = time_us_32();

	for (;;)
	{
		uint32_t NowUs = time_us_32();
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());
		uint32_t FrameUs = NowUs - LastFrameUs;
		uint32_t T0;
		bool TextDue;
		bool Down;
		int32_t Tx, Ty;
		uint8_t Page;

		LastFrameUs = NowUs;

		Panel_Alive(PANEL_STAGE_UI_UPDATE);
		Panel_TouchService();
		UiGPage_Sample(FrameUs);
		UiGraphPage_Sample(NowMs, FrameUs);
		UiClockPage_Sample(NowMs, FrameUs);
		Page = Pages_Effective(NowMs);
		TextDue = (int32_t)(NowMs - NextValueMs) >= 0;
		if (TextDue)
			NextValueMs = NowMs + CORE1_VALUE_PERIOD_MS;

		/* ---- the speaker --------------------------------------------- *
		 *
		 * Decided here rather than on core 0 because the decision needs the
		 * page this node is actually showing: a node sounds a warning about a
		 * reading its own face is displaying. See warn.h.
		 */
		{
			WarnId_t Warned = Warn_Update(&Warn, NowMs, Page, DashNodeId);

			if (BeepRequest != 0xFFu)
			{
				BeepId = (ToneId_t)BeepRequest;
				BeepUntilMs = NowMs + BEEP_TEST_MS;
				BeepRequest = 0xFFu;
				printf("audio: %s\n", Tone_Name(BeepId));
			}
			if (VolumeRequest != 0xFFu)
			{
				Audio_SetVolume(VolumeRequest);
				VolumeRequest = 0xFFu;
				printf("audio: volume %lu%%\n", (unsigned long)Audio_Volume());
			}

			/* A console test sound outranks a real warning, but only for the
			   few seconds it was asked for - a bench test that could mask a
			   fault indefinitely would be the wrong trade. */
			if (BeepId != TONE_NONE && (int32_t)(NowMs - BeepUntilMs) < 0)
				Audio_Warn(BeepId);
			else
			{
				BeepId = TONE_NONE;
				Audio_Warn(Warn_Tone(Warned));
			}
		}

		/* ---- the finger ---------------------------------------------- */

		Down = Panel_TouchDown(&Tx, &Ty);
		if (Down && !Swipe.Down)
		{
			Swipe.X0 = Tx;
			Swipe.Y0 = Ty;
			Swipe.Samples = 0u;
			Swipe.LastReports = Panel_TouchReports();
			Core1SwipeSample(&Swipe, Tx, NowUs);
			Swipe.DownUs = NowUs;
			Swipe.Wander = 0;
			Swipe.Dragged = false;
			Swipe.LongDone = false;
		}
		if (Down)
		{
			if (Panel_TouchReports() != Swipe.LastReports)
			{
				Swipe.LastReports = Panel_TouchReports();
				Core1SwipeSample(&Swipe, Tx, NowUs);
			}
			Swipe.X = Tx;
			Swipe.Y = Ty;
			{
				int32_t Ax = (Tx > Swipe.X0) ? Tx - Swipe.X0 : Swipe.X0 - Tx;
				int32_t Ay = (Ty > Swipe.Y0) ? Ty - Swipe.Y0 : Swipe.Y0 - Ty;

				if (Ax > Swipe.Wander)
					Swipe.Wander = Ax;
				if (Ay > Swipe.Wander)
					Swipe.Wander = Ay;
			}
		}

		/* TAP AND LONG PRESS, on a g-force page only. A press that stays put -
		   within CORE1_TAP_WANDER_PX - and never became a swipe is a tap if it
		   lifts inside CORE1_TAP_MAX_US, and a long press once it has been down
		   CORE1_LONG_PRESS_US: the tap clears the peaks, the long press zeroes
		   the meter. The long press acts while the finger is still down, so
		   there is something to see before letting go. */
		if (Swipe.State == SWIPE_IDLE && Cur->Valid && Cur->HasG)
		{
			bool Still = Swipe.Wander <= CORE1_TAP_WANDER_PX && !Swipe.Dragged;

			if (Down && Swipe.Down && Still && !Swipe.LongDone
			    && (uint32_t)(NowUs - Swipe.DownUs) >= CORE1_LONG_PRESS_US)
			{
				Swipe.LongDone = true;
				if (UiGPage_Zero())
				{
					Counts.Zeroed++;
					printf("g-force: zeroed at rest\n");
				}
				else
				{
					Counts.ZeroRefused++;
					printf("g-force: not zeroed - the reading is not one of a car at rest\n");
				}
			}
			else if (!Down && Swipe.Down && Still && !Swipe.LongDone
			         && (uint32_t)(NowUs - Swipe.DownUs) <= CORE1_TAP_MAX_US)
			{
				UiGPage_ResetPeaks();
				Counts.Taps++;
			}
		}

		/* A fault ends any swipe where it stands. */
		if (Swipe.State != SWIPE_IDLE && Pages_WarningActive(NowMs))
			Swipe.State = SWIPE_IDLE;

		switch (Swipe.State)
		{
		case SWIPE_IDLE:
		{
			int32_t Dx = Swipe.X - Swipe.X0;
			int32_t Dy = Swipe.Y - Swipe.Y0;
			int32_t Ax = (Dx < 0) ? -Dx : Dx;
			int32_t Ay = (Dy < 0) ? -Dy : Dy;

			if (Down && Swipe.Down && Cur->Valid && !Pages_WarningActive(NowMs)
			    && Ax >= CORE1_DRAG_START_PX && Ax > 2 * Ay)
			{
				Swipe.Direction = (Dx < 0) ? 1 : -1;
				if (Pages_Neighbour(Swipe.Direction) != Cur->Page)
				{
					Panel_Alive(PANEL_STAGE_DRAW);
					Core1LoadPage(In, Pages_Neighbour(Swipe.Direction),
					              In->Surface, NowMs);
					Swipe.Revealed = 0;
					Swipe.Dragged = true;
					Swipe.State = SWIPE_DRAG;
				}
			}
			break;
		}

		case SWIPE_DRAG:
			if (Down)
			{
				int32_t Dx = Swipe.X - Swipe.X0;
				int32_t R = (Swipe.Direction > 0) ? -Dx : Dx;

				/* Dragged back past where it started: the other neighbour. */
				if (R < 0 && Pages_Neighbour(-Swipe.Direction) != Cur->Page)
				{
					Swipe.Direction = -Swipe.Direction;
					Core1LoadPage(In, Pages_Neighbour(Swipe.Direction),
					              In->Surface, NowMs);
					R = -R;
				}
				Swipe.Revealed = (R < 0) ? 0 : ((R > PANEL_WIDTH) ? PANEL_WIDTH : R);
			}
			else
			{
				int32_t Finger = Core1SwipeSpeed(&Swipe);
				int32_t Towards = (Swipe.Direction > 0) ? -Finger : Finger;
				int32_t Projected = Swipe.Revealed
				                    + (int32_t)(((int64_t)Towards * CORE1_PROJECT_US) / 1000000);
				int32_t Dist, MinSpeed, MaxSpeed, Speed;
				bool Complete;

				if (Towards >= CORE1_FLICK_PX_PER_S)
					Complete = true;
				else if (Towards <= -CORE1_FLICK_PX_PER_S)
					Complete = false;
				else
					Complete = Projected >= (PANEL_WIDTH / 2);

				Swipe.Target = Complete ? PANEL_WIDTH : 0;
				Swipe.From = Swipe.Revealed;
				Dist = Swipe.Target - Swipe.From;
				if (Dist < 0)
					Dist = -Dist;

				/* The finger's speed in the direction the page is now going,
				   held between what finishes within the longest settle and
				   what still takes the shortest. */
				Speed = Complete ? Towards : -Towards;
				MinSpeed = (int32_t)(((int64_t)2 * Dist * 1000000) / CORE1_SETTLE_MAX_US);
				MaxSpeed = (int32_t)(((int64_t)2 * Dist * 1000000) / CORE1_SETTLE_MIN_US);
				if (Speed < MinSpeed)
					Speed = MinSpeed;
				if (Speed > MaxSpeed)
					Speed = MaxSpeed;

				/* Constant deceleration from Speed to rest covers Dist in
				   2 x Dist / Speed. */
				Swipe.Speed = Speed;
				Swipe.DurationUs = (Speed > 0)
				                   ? (int32_t)(((int64_t)2 * Dist * 1000000) / Speed) : 0;
				Swipe.StartUs = NowUs;
				Swipe.State = SWIPE_SETTLE;
			}
			break;

		case SWIPE_SETTLE:
		{
			int64_t T = (int64_t)(uint32_t)(NowUs - Swipe.StartUs);

			if (T >= (int64_t)Swipe.DurationUs)
			{
				Swipe.Revealed = Swipe.Target;
			}
			else
			{
				/* s = v t - v t^2 / (2 D): speed v at the start, zero at D. */
				int64_t V = Swipe.Speed;
				int64_t D = Swipe.DurationUs;
				int64_t Travel = ((V * T) - ((V * T / D) * T) / 2) / 1000000;

				Swipe.Revealed = (Swipe.Target > Swipe.From)
				                 ? Swipe.From + (int32_t)Travel
				                 : Swipe.From - (int32_t)Travel;
			}
			break;
		}
		}
		Swipe.Down = Down;

		/* ---- the frame ----------------------------------------------- */

		T0 = time_us_32();

		if (Swipe.State != SWIPE_IDLE)
		{
			Core1Dirty_t Ignored = { 0u, { { 0, 0, 0, 0 } } };
			Core1Slide_t Slide;
			PanelSource_t Source;
			PanelPush_t Push;

			Panel_Alive(PANEL_STAGE_DRAW);
			Core1UpdatePage(Cur, NowMs, FrameUs, TextDue, &Ignored, &Counts);
			Ignored.Count = 0u;
			Core1UpdatePage(In, NowMs, FrameUs, TextDue, &Ignored, &Counts);

			if (Swipe.Direction > 0)
			{
				Slide.Left = UiDraw_Buffer(Cur->Surface);
				Slide.Right = UiDraw_Buffer(In->Surface);
				Slide.Offset = Swipe.Revealed;
			}
			else
			{
				Slide.Left = UiDraw_Buffer(In->Surface);
				Slide.Right = UiDraw_Buffer(Cur->Surface);
				Slide.Offset = PANEL_WIDTH - Swipe.Revealed;
			}
			Source.Row = Core1SlideRow;
			Source.Context = &Slide;
			Source.Palette = UiDraw_Palette();

			DrawUs = time_us_32() - T0;
			Panel_Alive(PANEL_STAGE_PUSH);
			Panel_PushRows(&Source, 0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
			Counts.Slides++;
			PushPixels = (uint32_t)PANEL_WIDTH * (uint32_t)PANEL_HEIGHT;

			Panel_Push(&Push);
			WorkUs = DrawUs + Push.LastTotalUs;
			if (WorkUs > SlideWorkMaxUs)
				SlideWorkMaxUs = WorkUs;

			/* Settled: the screen already shows where it ended. */
			if (Swipe.State == SWIPE_SETTLE && Swipe.Revealed == Swipe.Target)
			{
				if (Swipe.Target == PANEL_WIDTH)
				{
					Core1Page_t *Was = Cur;

					if (Swipe.Direction > 0)
						Pages_Next();
					else
						Pages_Previous();
					Cur = In;
					In = Was;
					Counts.Swipes++;
				}
				else
				{
					Counts.Cancels++;
				}
				Swipe.State = SWIPE_IDLE;
			}
		}
		else if (!Started || Page != Cur->Page)
		{
			/* A page arrived by some other road - start-up, or the fault
			   takeover - so it is drawn whole and sent whole. */
			Panel_Alive(PANEL_STAGE_DRAW);
			Started = true;
			Core1LoadPage(Cur, Page, Cur->Surface, NowMs);
			if (Cur->Valid)
				printf("page %u: face copied in %luus, %u gauge(s), palette %u of %u\n",
				       Page, (unsigned long)UiDraw_LoadUs(), Cur->GaugeCount,
				       UiDraw_PaletteUsed(), (unsigned)UI_DRAW_PALETTE_MAX);

			Panel_Alive(PANEL_STAGE_PUSH);
			Panel_PushPaletted(UiDraw_Buffer(Cur->Surface), UI_DRAW_WIDTH,
			                   UiDraw_Palette(),
			                   0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
		}
		else if (Cur->Valid)
		{
			Core1Dirty_t Dirty = { 0u, { { 0, 0, 0, 0 } } };

			Panel_Alive(PANEL_STAGE_DRAW);
			Core1UpdatePage(Cur, NowMs, FrameUs, TextDue, &Dirty, &Counts);

			if (Dirty.Count == 0u)
			{
				Counts.Still++;
				Panel_WaitFrame();
			}
			else
			{
				PanelPush_t Push;
				uint32_t i;

				DrawUs = time_us_32() - T0;
				PushPixels = 0;
				for (i = 0; i < Dirty.Count; i++)
					PushPixels += (uint32_t)((Dirty.Rects[i].X2 - Dirty.Rects[i].X1 + 1)
					                         * (Dirty.Rects[i].Y2 - Dirty.Rects[i].Y1 + 1));

				Panel_Alive(PANEL_STAGE_PUSH);
				Panel_PushRegions(UiDraw_Buffer(Cur->Surface), UI_DRAW_WIDTH,
				                  UiDraw_Palette(), Dirty.Rects, Dirty.Count);

				/* The frame's own work: drawing, plus the push measured from
				   the TE edge - what has to fit inside one scan. */
				Panel_Push(&Push);
				WorkUs = DrawUs + Push.LastTotalUs;
				if (WorkUs > WorkMaxUs)
					WorkMaxUs = WorkUs;
			}
		}
		else
		{
			Panel_WaitFrame();
		}

		if (ScreenshotRequested)
		{
			ScreenshotRequested = false;
			Core1Screenshot(Cur->Surface);
		}

		if (TimeRequested)
		{
			RtcTime_t T;

			T.Hours = TimeRequest[0];
			T.Minutes = TimeRequest[1];
			T.Seconds = TimeRequest[2];
			TimeRequested = false;

			if (Rtc_Write(&T))
				printf("rtc: set to %02u:%02u:%02u\n", T.Hours, T.Minutes, T.Seconds);
			else
				printf("rtc: refused %02u:%02u:%02u - not a time, or no clock\n",
				       T.Hours, T.Minutes, T.Seconds);
		}

		/* A console page step changes page as the fault takeover does -
		   instantly, without a slide - and only when no swipe is under way. */
		if (PageStepRequested != 0 && Swipe.State == SWIPE_IDLE)
		{
			if (PageStepRequested > 0)
				Pages_Next();
			else
				Pages_Previous();
			PageStepRequested = 0;
		}

		if ((int32_t)(NowMs - NextStatusMs) >= 0)
		{
			uint16_t TouchX;
			uint16_t TouchY;

			NextStatusMs = NowMs + STATUS_PERIOD_MS;
			Panel_TouchLast(&TouchX, &TouchY);

#if DASH_SIMULATE
			/* The store reports the link alive on simulated values - say so
			   rather than print "link up" for a bus nobody is talking on. */
			const char *LinkText = "SIMULATED";
#else
			const char *LinkText = SignalStore_LinkAlive(NowMs) ? "link up"
			                                                    : "LINK DOWN";
#endif

			{
				PanelPush_t Push;

				Panel_Push(&Push);
				uint32_t LiftReports, LiftTimeouts, GapMaxMs, ReadErrors;
				uint16_t StatusSeen;

				Panel_TouchLifts(&LiftReports, &LiftTimeouts, &GapMaxMs,
				                 &StatusSeen, &ReadErrors);
				{
					RtcTime_t T;

					if (!Rtc_Present())
						printf("rtc: not found\n");
					else if (UiClockPage_Time(&T))
						printf("rtc: %02u:%02u:%02u  from the bus %lu  rejected %lu"
						       "  errors %lu\n", T.Hours, T.Minutes, T.Seconds,
						       (unsigned long)ClockLink_Accepted(),
						       (unsigned long)ClockLink_Rejected(),
						       (unsigned long)Rtc_Errors());
					else
						printf("rtc: no trustworthy time - set it with Thhmmss, or "
						       "announce it on 0x%03x  rejected %lu  errors %lu\n",
						       (unsigned)CLOCK_LINK_TIME_ID,
						       (unsigned long)ClockLink_Rejected(),
						       (unsigned long)Rtc_Errors());
				}
				if (!Audio_Present())
					printf("audio: no codec\n");
				else
					printf("audio: 0x%04x  %s  vol %lu%%  buffers %lu  late %lu"
					       "  fired fault %lu rev %lu boost %lu lean %lu\n",
					       (unsigned)Audio_ChipId(),
					       Tone_Name(Audio_Sounding()),
					       (unsigned long)Audio_Volume(),
					       (unsigned long)Audio_Buffers(),
					       (unsigned long)Audio_Underruns(),
					       (unsigned long)Warn.Fired[WARN_FAULT],
					       (unsigned long)Warn.Fired[WARN_REV],
					       (unsigned long)Warn.Fired[WARN_BOOST],
					       (unsigned long)Warn.Fired[WARN_MIXTURE]);
				{
					ImuMilliG_t A;

					if (!Imu_Present())
						printf("imu: not found\n");
					else if (!Imu_Read(&A))
						printf("imu: read failed (%lu so far)\n",
						       (unsigned long)Imu_ReadErrors());
					else
					{
						/* Integer square root of the magnitude, in mg. */
						uint64_t Sq = (uint64_t)((int64_t)A.X * A.X)
						              + (uint64_t)((int64_t)A.Y * A.Y)
						              + (uint64_t)((int64_t)A.Z * A.Z);
						uint32_t Mag = 0;
						uint32_t Bit = 1u << 30;

						while (Bit > Sq)
							Bit >>= 2;
						while (Bit != 0u)
						{
							if (Sq >= (uint64_t)Mag + Bit)
							{
								Sq -= (uint64_t)Mag + Bit;
								Mag = (Mag >> 1) + Bit;
							}
							else
							{
								Mag >>= 1;
							}
							Bit >>= 2;
						}

						const GMeter_t *Gm = UiGPage_Model();
						const GMeterCal_t *Gc = UiGPage_Calibration();

						printf("g: lat %ld lon %ld mg  peaks R %ld L %ld A %ld B %ld mg"
						       "  %s rest %ld,%ld,%ld  taps %lu  zeroed %lu refused %lu\n",
						       (long)lroundf(Gm->Lat * 1000.0f), (long)lroundf(Gm->Lon * 1000.0f),
						       (long)lroundf(Gm->PeakRight * 1000.0f),
						       (long)lroundf(Gm->PeakLeft * 1000.0f),
						       (long)lroundf(Gm->PeakAccel * 1000.0f),
						       (long)lroundf(Gm->PeakBrake * 1000.0f),
						       Gc->Zeroed ? "zeroed" : "ASSUMED UPRIGHT",
						       (long)Gc->Rest.X, (long)Gc->Rest.Y, (long)Gc->Rest.Z,
						       (unsigned long)Counts.Taps, (unsigned long)Counts.Zeroed,
						       (unsigned long)Counts.ZeroRefused);
						printf("imu: 0x%02x rev 0x%02x  x %ld  y %ld  z %ld mg"
						       "  |a| %lu mg  errors %lu\n",
						       Imu_Address(), Imu_Revision(), (long)A.X, (long)A.Y,
						       (long)A.Z, (unsigned long)Mag,
						       (unsigned long)Imu_ReadErrors());
					}
				}
				printf("touch lifts: by report %lu  by timeout %lu  longest report gap %lums"
				       "  statuses seen 0x%04x  read errors %lu\n",
				       (unsigned long)LiftReports, (unsigned long)LiftTimeouts,
				       (unsigned long)GapMaxMs, (unsigned)StatusSeen,
				       (unsigned long)ReadErrors);
				printf("frames: needles %lu  values %lu  still %lu  slides %lu"
				       "  swipes %lu  cancels %lu  |  last: draw %luus  %lupx in %lu"
				       "  work %luus  worst %luus  worst slide %luus  short rows %lu\n",
				       (unsigned long)Counts.Needles, (unsigned long)Counts.Values,
				       (unsigned long)Counts.Still, (unsigned long)Counts.Slides,
				       (unsigned long)Counts.Swipes, (unsigned long)Counts.Cancels,
				       (unsigned long)DrawUs, (unsigned long)PushPixels,
				       (unsigned long)Push.LastRegions, (unsigned long)WorkUs,
				       (unsigned long)WorkMaxUs, (unsigned long)SlideWorkMaxUs,
				       (unsigned long)Push.RowShortfalls);
				WorkMaxUs = 0;
				SlideWorkMaxUs = 0;
			}
			printf("node %u  page %u  %s  flush %lu  "
			       "touch %s rep %lu press %lu @%u,%u",
			       DashNodeId, Page, LinkText,
			       (unsigned long)Panel_Flushes(),
			       Panel_TouchPresent() ? "ok" : "ABSENT",
			       (unsigned long)Panel_TouchReports(),
			       (unsigned long)Panel_TouchPresses(), TouchX, TouchY);
			printf("  int %lu up / %lu down",
			       (unsigned long)Panel_TouchRiseEdges(),
			       (unsigned long)Panel_TouchFallEdges());
			{
				PanelPush_t Push;

				Panel_Push(&Push);
				printf("\n  push %lu  chunks %lu  starved %lu"
				       "  last %luus = convert %lu + blocked %lu",
				       (unsigned long)Push.Frames,
				       (unsigned long)Push.Chunks,
				       (unsigned long)Push.Starved,
				       (unsigned long)Push.LastTotalUs,
				       (unsigned long)Push.LastConvertUs,
				       (unsigned long)Push.LastBlockedUs);
			}
			{
				PanelTe_t Te;

				Panel_Te(&Te);
				printf("\n  te %s  edges %lu  period %luus  waits %lu"
				       "  timeouts %lu  avg wait %luus  late %lu",
				       Te.Enabled ? "on" : "OFF (no edges)",
				       (unsigned long)Te.Edges, (unsigned long)Te.PeriodUs,
				       (unsigned long)Te.Waits, (unsigned long)Te.Timeouts,
				       (unsigned long)Te.AvgWaitUs,
				       (unsigned long)Te.LateFrames);
			}
			/* 32-bit on purpose. %llu printed garbage at -O2 - the value read
			   a word out - and the low half of a microsecond counter does not
			   wrap for 71 minutes, so it is enough to diff over a benchmark
			   window. */
			printf("\n  bench us %lu px %lu",
			       (unsigned long)(Panel_RenderTotalUs() & 0xFFFFFFFFu),
			       (unsigned long)(Panel_RenderTotalPx() & 0xFFFFFFFFu));
			printf("\n  rounded %lu  overlap dma %lu cs %lu",
			       (unsigned long)Panel_RoundedAreas(),
			       (unsigned long)Panel_FlushOverlaps(),
			       (unsigned long)Panel_FlushCsOverlaps());
			printf("\n  bus %lu.%lu MB/s  %luus/frame  drain<=%lu",
			       (unsigned long)(Panel_FlushMbPerSx10() / 10u),
			       (unsigned long)(Panel_FlushMbPerSx10() % 10u),
			       (unsigned long)Panel_FlushBusyUsPerFrame(),
			       (unsigned long)Panel_DrainSpinsMax());
			if (Panel_FlushTimeouts() != 0u)
				printf("  flush-timeout %lu",
				       (unsigned long)Panel_FlushTimeouts());
			printf("\n");

			Core1ReportFace(NowMs, Page, false);
		}
	}
}
#else
/***************************************************************************************/
/* Core 1 without a display layer: report what would have been drawn. */
static void Core1Main(void)
{
	uint8_t LastPage = 0xFF;

	for (;;)
	{
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());
		uint8_t Page = Pages_Effective(NowMs);

		if (Page != LastPage)
		{
			printf("\n-- page %u: %s%s --\n", Page, Pages[Page].Name,
			       Pages_WarningActive(NowMs) ? "  (FAULT TAKEOVER)" : "");
			LastPage = Page;
		}

		Core1ReportFace(NowMs, Page, true);

		sleep_ms(500);
	}
}
#endif


/***************************************************************************************/
int main(void)
{
	uint8_t Id;
#if DASH_SIMULATE
	uint32_t NextSimMs = 0;
#endif
#if DASH_HAVE_PANEL
	uint32_t NextStageReportMs = 0;
	uint32_t LastAliveCount = 0;
#endif

#if DASH_HAVE_PANEL
	/* Before stdio and before CanLink_Init(): this raises the system clock to
	   the frequency the panel's PIO divider is chosen against, and can2040
	   computes its bit timing from clock_get_hz(clk_sys) once, at startup. A
	   clock change after that point would put every CAN bit at the wrong
	   length with nothing to show for it but errors. */
	Panel_ClockInit();
#endif

	stdio_init_all();

	SignalStore_Init();
	NodeId_Init();

	Id = NodeId_Get();

	/* An unreadable divider must not stop the node booting - a blank gauge
	   tells the driver nothing. Start as node 0 and make the fault visible
	   instead. */
	if (Id == NODE_ID_UNKNOWN)
	{
		printf("node identity unreadable - check the divider; assuming 0\n");
		Id = 0;
	}
	DashNodeId = Id;
	printf("dash node %u starting\n", Id);

	/* 0xFF: no stored selection yet. Flash-backed persistence is still to be
	   written; until then every boot starts on the node's startup page. */
	Pages_Init(Id, 0xFF);

	Telemetry_Init(TELEMETRY_BASE_CPU1);

#if DASH_SIMULATE
	printf("SIMULATED TELEMETRY BUILD - RPM and boost are synthetic, not from "
	       "the ECU. Rebuild with -DDASH_SIMULATE=OFF for real data.\n");
#endif

#if DASH_HAVE_CAN2040
	CanLink_Init(Id);
#else
	printf("built without can2040 - see README; no telemetry will arrive\n");
#endif

#if DASH_HAVE_PANEL
	multicore_launch_core1_with_stack(Core1Main, Core1Stack,
	                                  sizeof(Core1Stack));
#else
	multicore_launch_core1(Core1Main);
#endif

	for (;;)
	{
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());

#if DASH_HAVE_CAN2040
		CanLink_Poll(NowMs);
#endif

#if DASH_HAVE_PANEL
		/* Console commands: 'S' a screenshot, 'n' and 'p' the next and
		   previous page, "Thhmmss" to set the clock, '1'..'4' to sound each
		   warning and '0' to stop, "Vnn" for the volume - enough to drive the
		   display, the clock and the speaker from the bench PC. The sounds
		   matter most: nothing else here can prove a speaker works without an
		   engine to make it go off. */
		{
			static uint8_t Digits[6];
			static uint8_t Have;
			static uint8_t Want;		/* digits this command is collecting */
			static char Collecting;

			int Ch = getchar_timeout_us(0);

			if (Collecting != 0)
			{
				if (Ch >= '0' && Ch <= '9')
				{
					Digits[Have++] = (uint8_t)(Ch - '0');
					if (Have == Want)
					{
						if (Collecting == 'T')
						{
							TimeRequest[0] = (uint8_t)((Digits[0] * 10u) + Digits[1]);
							TimeRequest[1] = (uint8_t)((Digits[2] * 10u) + Digits[3]);
							TimeRequest[2] = (uint8_t)((Digits[4] * 10u) + Digits[5]);
							TimeRequested = true;
						}
						else
						{
							VolumeRequest = (uint8_t)((Digits[0] * 10u) + Digits[1]);
						}
						Collecting = 0;
					}
				}
				else if (Ch != PICO_ERROR_TIMEOUT)
				{
					/* Anything else abandons it rather than waiting for
					   digits that may never come. */
					Collecting = 0;
				}
			}
			else if (Ch == 'S')
				ScreenshotRequested = true;
			else if (Ch == 'n')
				PageStepRequested = 1;
			else if (Ch == 'p')
				PageStepRequested = -1;
			else if (Ch == 'T')
			{
				Collecting = 'T';
				Want = 6u;
				Have = 0u;
			}
			else if (Ch == 'V')
			{
				Collecting = 'V';
				Want = 2u;
				Have = 0u;
			}
			else if (Ch >= '0' && Ch <= '0' + (int)TONE_COUNT - 1)
				BeepRequest = (uint8_t)(Ch - '0');
		}
#endif

#if DASH_SIMULATE
		if ((int32_t)(NowMs - NextSimMs) >= 0)
		{
			NextSimMs = NowMs + SIM_PERIOD_MS;
			Simulate(NowMs);
		}
#endif

#if DASH_HAVE_PANEL
		/* Watch core 1 for a stall. Core 0 is the one that cannot hang here -
		   it owns USB - so this is the only place from which a wedged renderer
		   can be seen at all.

		   Driven by whether core 1's counter has moved rather than by a flag
		   saying it started, because those are different questions: the first
		   version of this went quiet the moment core 1 reported itself running,
		   and then said nothing when core 1 hung a few instructions later. */
		if ((int32_t)(NowMs - NextStageReportMs) >= 0)
		{
			uint32_t Alive = Panel_AliveCount();

			NextStageReportMs = NowMs + STAGE_REPORT_PERIOD_MS;

			if (Alive == LastAliveCount)
				printf("CORE 1 STALLED at stage %u (%s), %lu loops, %lums\n",
				       Panel_Stage(), Panel_StageName(Panel_Stage()),
				       (unsigned long)Alive, (unsigned long)NowMs);

			LastAliveCount = Alive;
		}
#endif
		tight_loop_contents();
	}
}
