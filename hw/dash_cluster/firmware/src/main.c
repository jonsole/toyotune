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
#include "panel.h"
#include "ui_draw.h"
#include "ui_gauge.h"
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
   and reading. */
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
	const DashFace_t *Face;
	uint8_t GaugeCount;
	Core1Gauge_t Gauges[CORE1_MAX_GAUGES];
} Core1Page_t;

static bool Core1FindPage(uint8_t Page, Core1Page_t *Out)
{
	uint8_t e, i;

	memset(Out, 0, sizeof(*Out));

	for (i = 0; i < DashFaceCount; i++)
		if (DashFaces[i].Page == Page)
			Out->Face = &DashFaces[i];

	for (e = 0; e < Pages[Page].ElementCount && Out->GaugeCount < CORE1_MAX_GAUGES; e++)
	{
		const FaceElement_t *El = &Pages[Page].Elements[e];
		Core1Gauge_t *G;

		if (El->Type != WIDGET_GAUGE)
			continue;

		G = &Out->Gauges[Out->GaugeCount++];
		G->Element = El;
		G->X = UiGauge_Pct(El->X, PANEL_WIDTH);
		G->Y = UiGauge_Pct(El->Y, PANEL_HEIGHT);
		G->W = UiGauge_Pct(El->W, PANEL_WIDTH);
		G->H = UiGauge_Pct(El->H, PANEL_HEIGHT);
	}

	return Out->Face != NULL && Out->GaugeCount != 0u;
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
/* The dirty rectangle a frame accumulates. */
typedef struct
{
	bool Any;
	UiRect_t Rect;
} Core1Dirty_t;

static void Core1Dirty(Core1Dirty_t *D, const UiRect_t *R)
{
	D->Rect = D->Any ? UiRect_Union(&D->Rect, R) : *R;
	D->Any = true;
}


/***************************************************************************************/
/* One gauge's frame: ease its needle and refresh its reading, restoring the
   face under whatever moved and adding it to the frame's dirty rectangle.

   NOTHING ON A FACE OVERLAPS ANYTHING ELSE LIVE: readings sit inside the centre
   ring, needles start outside it, and a split face's two needles keep to their
   own halves. So each item can be restored and redrawn without disturbing the
   others, and one rectangle round everything that changed is all that has to
   be sent. */
static void Core1UpdateGauge(Core1Gauge_t *G, uint32_t NowMs, uint32_t FrameUs,
                             bool TextDue, Core1Dirty_t *Dirty,
                             uint32_t *NeedleFrames, uint32_t *ValueFrames)
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
			UiDraw_Restore(&G->TextRect);
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
			(void)UiDraw_Text(&dash_font_value_56, G->Text, Tx, Ty);
			G->TextRect = Ink;
			Core1Dirty(Dirty, &Ink);
		}
		(*ValueFrames)++;
	}

	if (!G->HaveNeedle || !UiNeedle_Same(&Next, &G->Needle))
	{
		UiRect_t NextRect = UiNeedle_Bounds(&Next);

		if (G->HaveNeedle)
		{
			UiDraw_Restore(&G->NeedleRect);
			Core1Dirty(Dirty, &G->NeedleRect);
		}
		(void)UiDraw_Needle(&Next);
		Core1Dirty(Dirty, &NextRect);

		G->Needle = Next;
		G->NeedleRect = NextRect;
		G->HaveNeedle = true;
		(*NeedleFrames)++;
	}
}


/***************************************************************************************/
/* SWIPING, FOR NOW WITHOUT ANIMATION.
 *
 * A deliberate horizontal gesture changes page when the finger lifts: it must
 * travel CORE1_SWIPE_MIN_PX, and at least twice as far across as up or down, so
 * a tap or a vertical brush does nothing. Finger moving left brings in the
 * next page, as on a phone. */
#define CORE1_SWIPE_MIN_PX	(80)

typedef struct
{
	bool Down;
	int32_t X0, Y0, X, Y;
} Core1Touch_t;

/* -1 previous, +1 next, 0 nothing. */
static int Core1Swipe(Core1Touch_t *T)
{
	int32_t X, Y, Dx, Dy;
	bool Down = Panel_TouchDown(&X, &Y);

	if (Down && !T->Down)
	{
		T->X0 = X;
		T->Y0 = Y;
	}
	if (Down)
	{
		T->X = X;
		T->Y = Y;
	}

	if (!(!Down && T->Down))
	{
		T->Down = Down;
		return 0;
	}

	T->Down = false;
	Dx = T->X - T->X0;
	Dy = T->Y - T->Y0;
	if ((Dx < 0 ? -Dx : Dx) < CORE1_SWIPE_MIN_PX
	    || (Dx < 0 ? -Dx : Dx) < 2 * (Dy < 0 ? -Dy : Dy))
		return 0;
	return (Dx < 0) ? 1 : -1;
}


/***************************************************************************************/
/* Core 1: the display, one panel frame per loop.
 *
 * The loop is clocked by the panel. Panel_PushPaletted() waits for the TE
 * pulse and returns once the last pixel has been clocked out, and when there
 * is nothing to send Panel_WaitFrame() waits for the pulse instead - so the
 * loop runs at the scan rate with no timer in it anywhere.
 *
 * A page change sends the whole screen. After that, each frame updates every
 * gauge on the page and sends one rectangle round everything that changed.
 *
 * Status goes out every STATUS_PERIOD_MS from here too. Printing it costs a
 * frame, once every two seconds. */
static void Core1Main(void)
{
	uint32_t NextStatusMs = 0;
	uint8_t LastPage = 0xFFu;
	uint32_t LastFrameUs;
	Core1Page_t View;
	bool HaveView = false;
	Core1Touch_t Touch = { false, 0, 0, 0, 0 };
	uint32_t NextValueMs = 0;
	uint32_t NeedleFrames = 0, ValueFrames = 0, StillFrames = 0, Swipes = 0;
	uint32_t DrawUs = 0, PushPixels = 0, WorkUs = 0, WorkMaxUs = 0;
	const char *LastText = "";

	Panel_Init();
	UiDraw_Init();
	LastFrameUs = time_us_32();

	for (;;)
	{
		uint32_t NowUs = time_us_32();
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());
		uint32_t FrameUs = NowUs - LastFrameUs;
		uint8_t Page;
		int Swipe;

		LastFrameUs = NowUs;

		Panel_Alive(PANEL_STAGE_UI_UPDATE);
		Panel_TouchService();
		Swipe = Core1Swipe(&Touch);
		if (Swipe > 0)
			Pages_Next();
		else if (Swipe < 0)
			Pages_Previous();
		if (Swipe != 0)
			Swipes++;

		Page = Pages_Effective(NowMs);

		if (Page != LastPage)
		{
			uint8_t g;

			Panel_Alive(PANEL_STAGE_DRAW);
			UiDraw_Init();
			HaveView = Core1FindPage(Page, &View);

			if (!HaveView)
				printf("page %u: no pre-rendered gauges - black\n", Page);
			else if (UiDraw_LoadFace(View.Face, View.Gauges[0].X, View.Gauges[0].Y))
				printf("page %u: face copied in %luus, %u gauge(s), palette %u of %u\n",
				       Page, (unsigned long)UiDraw_LoadUs(), View.GaugeCount,
				       UiDraw_PaletteUsed(), (unsigned)UI_DRAW_PALETTE_MAX);
			else
			{
				printf("page %u: %ldx%ld face does not fit at %ld,%ld - black\n",
				       Page, (long)View.Face->Width, (long)View.Face->Height,
				       (long)View.Gauges[0].X, (long)View.Gauges[0].Y);
				HaveView = false;
			}

			/* A new face starts with its needles on their readings, not
			   swinging up from zero. The readings are drawn by the first
			   ordinary frame, below. */
			for (g = 0; HaveView && g < View.GaugeCount; g++)
			{
				Core1Gauge_t *G = &View.Gauges[g];
				UiWidget_t W = UiModel_Widget(G->Element, NowMs);

				G->SmoothQ = (uint32_t)W.Position << UI_NEEDLE_Q;
				G->Needle = Core1Needle(G, G->SmoothQ);
				G->NeedleRect = UiNeedle_Bounds(&G->Needle);
				(void)UiDraw_Needle(&G->Needle);
				G->HaveNeedle = true;
			}

			Panel_Alive(PANEL_STAGE_PUSH);
			Panel_PushPaletted(UiDraw_Buffer(), UI_DRAW_WIDTH, UiDraw_Palette(),
			                   0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
			LastPage = Page;
			NextValueMs = NowMs;
		}
		else if (HaveView)
		{
			uint32_t T0 = time_us_32();
			bool TextDue = (int32_t)(NowMs - NextValueMs) >= 0;
			Core1Dirty_t Dirty = { false, { 0, 0, 0, 0 } };
			uint8_t g;

			Panel_Alive(PANEL_STAGE_DRAW);
			for (g = 0; g < View.GaugeCount; g++)
				Core1UpdateGauge(&View.Gauges[g], NowMs, FrameUs, TextDue, &Dirty,
				                 &NeedleFrames, &ValueFrames);
			if (TextDue)
				NextValueMs = NowMs + CORE1_VALUE_PERIOD_MS;
			LastText = View.Gauges[0].Text;

			if (!Dirty.Any)
			{
				StillFrames++;
				Panel_WaitFrame();
			}
			else
			{
				PanelPush_t Push;
				UiRect_t *R = &Dirty.Rect;

				DrawUs = time_us_32() - T0;
				PushPixels = (uint32_t)((R->X2 - R->X1 + 1) * (R->Y2 - R->Y1 + 1));

				Panel_Alive(PANEL_STAGE_PUSH);
				Panel_PushPaletted(UiDraw_Buffer(), UI_DRAW_WIDTH, UiDraw_Palette(),
				                   R->X1, R->Y1, R->X2, R->Y2);

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

			printf("frames: needles %lu  values %lu  still %lu  swipes %lu  |  last: draw %luus"
			       "  rect %lupx  work %luus  worst work %luus  \"%s\"\n",
			       (unsigned long)NeedleFrames, (unsigned long)ValueFrames,
			       (unsigned long)StillFrames, (unsigned long)Swipes,
			       (unsigned long)DrawUs, (unsigned long)PushPixels,
			       (unsigned long)WorkUs, (unsigned long)WorkMaxUs, LastText);
			WorkMaxUs = 0;
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
