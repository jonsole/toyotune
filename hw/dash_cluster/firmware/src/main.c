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
/* The page's gauge: the first gauge element, with its pre-rendered face. Every
   page has one gauge for now; when a page carries more, this becomes a list and
   each gets its own needle. */
typedef struct
{
	const FaceElement_t *Element;
	const DashFace_t *Face;
	int32_t X, Y, W, H;		/* the element, in panel pixels */
} Core1Gauge_t;

static bool Core1FindGauge(uint8_t Page, Core1Gauge_t *Out)
{
	uint8_t e, i;

	for (e = 0; e < Pages[Page].ElementCount; e++)
	{
		const FaceElement_t *El = &Pages[Page].Elements[e];

		if (El->Type != WIDGET_GAUGE)
			continue;

		for (i = 0; i < DashFaceCount; i++)
		{
			if (DashFaces[i].Page == Page && DashFaces[i].Element == e)
			{
				Out->Element = El;
				Out->Face = &DashFaces[i];
				Out->X = UiGauge_Pct(El->X, PANEL_WIDTH);
				Out->Y = UiGauge_Pct(El->Y, PANEL_HEIGHT);
				Out->W = UiGauge_Pct(El->W, PANEL_WIDTH);
				Out->H = UiGauge_Pct(El->H, PANEL_HEIGHT);
				return true;
			}
		}
	}

	return false;
}


/***************************************************************************************/
/* The needle for a gauge at an eased position. The pivot is the element's
   true centre - half-pixel and all, see ui_needle.h - which is where the face
   renderer put the centre of the dial. */
static UiNeedle_t Core1Needle(const Core1Gauge_t *G, uint32_t PositionQ)
{
	return UiNeedle_Place((float)G->X + ((float)G->W / 2.0f),
	                      (float)G->Y + ((float)G->H / 2.0f),
	                      (float)UiGauge_NeedleInner(G->W, G->H),
	                      (float)UiGauge_NeedleOuter(G->W, G->H),
	                      (float)UI_GAUGE_NEEDLE_WIDTH / 2.0f,
	                      PositionQ);
}


/***************************************************************************************/
/* Core 1: the display, one panel frame per loop.
 *
 * The loop is clocked by the panel. Panel_PushPaletted() waits for the TE
 * pulse and returns once the last pixel has been clocked out, and when there
 * is nothing to send Panel_WaitFrame() waits for the pulse instead - so the
 * loop runs at the scan rate with no timer in it anywhere.
 *
 * A page change sends the whole screen. After that, each frame eases the
 * needle towards its reading by however long the last frame took, and when it
 * has moved: puts the face back where it was, draws it where it is, and sends
 * only the rectangle covering both.
 *
 * Status goes out every STATUS_PERIOD_MS from here too. Printing it costs a
 * frame, once every two seconds. */
static void Core1Main(void)
{
	uint32_t NextStatusMs = 0;
	uint8_t LastPage = 0xFFu;
	uint32_t LastFrameUs;
	Core1Gauge_t Gauge;
	bool HaveGauge = false;
	bool HaveNeedle = false;
	UiNeedle_t Needle;
	UiRect_t NeedleRect = { 0, 0, 0, 0 };
	uint32_t SmoothQ = 0;
	char Text[sizeof(((UiWidget_t *)0)->Text)] = "";
	bool HaveText = false;
	UiRect_t TextRect = { 0, 0, 0, 0 };
	uint32_t NextValueMs = 0;
	uint32_t NeedleFrames = 0, ValueFrames = 0, StillFrames = 0;
	uint32_t DrawUs = 0, PushPixels = 0, WorkUs = 0, WorkMaxUs = 0;

	Panel_Init();
	UiDraw_Init();
	LastFrameUs = time_us_32();

	for (;;)
	{
		uint32_t NowUs = time_us_32();
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());
		uint32_t FrameUs = NowUs - LastFrameUs;
		uint8_t Page = Pages_Effective(NowMs);

		LastFrameUs = NowUs;

		Panel_Alive(PANEL_STAGE_UI_UPDATE);
		Panel_TouchService();

		if (Page != LastPage)
		{
			Panel_Alive(PANEL_STAGE_DRAW);
			UiDraw_Init();
			HaveGauge = Core1FindGauge(Page, &Gauge);
			HaveNeedle = false;
			HaveText = false;
			Text[0] = '\0';

			if (!HaveGauge)
				printf("face p%u: no pre-rendered gauge - black\n", Page);
			else if (UiDraw_LoadFace(Gauge.Face, Gauge.X, Gauge.Y))
				printf("face p%u: copied in %luus, palette %u of %u\n",
				       Page, (unsigned long)UiDraw_LoadUs(),
				       UiDraw_PaletteUsed(), (unsigned)UI_DRAW_PALETTE_MAX);
			else
			{
				printf("face p%u: %ldx%ld does not fit at %ld,%ld - black\n",
				       Page, (long)Gauge.Face->Width, (long)Gauge.Face->Height,
				       (long)Gauge.X, (long)Gauge.Y);
				HaveGauge = false;
			}

			/* A new face starts with its needle on the reading, not swinging
			   up from zero. The reading itself is drawn by the first ordinary
			   frame, below. */
			if (HaveGauge)
			{
				UiWidget_t W = UiModel_Widget(Gauge.Element, NowMs);

				SmoothQ = (uint32_t)W.Position << UI_NEEDLE_Q;
				Needle = Core1Needle(&Gauge, SmoothQ);
				NeedleRect = UiNeedle_Bounds(&Needle);
				(void)UiDraw_Needle(&Needle);
				HaveNeedle = true;
			}

			Panel_Alive(PANEL_STAGE_PUSH);
			Panel_PushPaletted(UiDraw_Buffer(), UI_DRAW_WIDTH, UiDraw_Palette(),
			                   0, 0, PANEL_WIDTH - 1, PANEL_HEIGHT - 1);
			LastPage = Page;
			NextValueMs = NowMs;
		}
		else if (HaveGauge)
		{
			UiWidget_t W = UiModel_Widget(Gauge.Element, NowMs);
			uint32_t T0 = time_us_32();
			UiNeedle_t Next;
			bool NeedleMoved, TextChanged;
			bool HaveDirty = false;
			UiRect_t Dirty = { 0, 0, 0, 0 };

			Panel_Alive(PANEL_STAGE_DRAW);
			SmoothQ = UiModel_NeedleStep(SmoothQ, W.Position, FrameUs);
			Next = Core1Needle(&Gauge, SmoothQ);
			NeedleMoved = !HaveNeedle || !UiNeedle_Same(&Next, &Needle);
			TextChanged = (int32_t)(NowMs - NextValueMs) >= 0
			              && strcmp(W.Text, Text) != 0;

			/* THE READING AND THE NEEDLE NEVER OVERLAP: the reading is inside
			   the centre ring and the needle starts outside it. So each can be
			   restored and redrawn without disturbing the other, and one
			   rectangle round both is all that has to be sent. */
			if (TextChanged)
			{
				int32_t Tx, Ty;
				UiRect_t Ink;

				if (HaveText)
				{
					UiDraw_Restore(&TextRect);
					Dirty = TextRect;
					HaveDirty = true;
				}

				(void)snprintf(Text, sizeof(Text), "%s", W.Text);
				UiText_Centre(&dash_font_value_56, Text,
				              (float)Gauge.X + ((float)Gauge.W / 2.0f),
				              (float)Gauge.Y + ((float)Gauge.H / 2.0f), &Tx, &Ty);
				HaveText = UiText_Bounds(&dash_font_value_56, Text, Tx, Ty, &Ink);
				if (HaveText)
				{
					(void)UiDraw_Text(&dash_font_value_56, Text, Tx, Ty);
					TextRect = Ink;
					Dirty = HaveDirty ? UiRect_Union(&Dirty, &Ink) : Ink;
					HaveDirty = true;
				}

				ValueFrames++;
				NextValueMs = NowMs + CORE1_VALUE_PERIOD_MS;
			}

			if (NeedleMoved)
			{
				UiRect_t NextRect = UiNeedle_Bounds(&Next);

				if (HaveNeedle)
				{
					UiDraw_Restore(&NeedleRect);
					Dirty = HaveDirty ? UiRect_Union(&Dirty, &NeedleRect) : NeedleRect;
					HaveDirty = true;
				}
				(void)UiDraw_Needle(&Next);
				Dirty = HaveDirty ? UiRect_Union(&Dirty, &NextRect) : NextRect;
				HaveDirty = true;

				Needle = Next;
				NeedleRect = NextRect;
				HaveNeedle = true;
				NeedleFrames++;
			}

			if (!HaveDirty)
			{
				StillFrames++;
				Panel_WaitFrame();
			}
			else
			{
				PanelPush_t Push;

				DrawUs = time_us_32() - T0;
				PushPixels = (uint32_t)((Dirty.X2 - Dirty.X1 + 1)
				                        * (Dirty.Y2 - Dirty.Y1 + 1));

				Panel_Alive(PANEL_STAGE_PUSH);
				Panel_PushPaletted(UiDraw_Buffer(), UI_DRAW_WIDTH,
				                   UiDraw_Palette(),
				                   Dirty.X1, Dirty.Y1, Dirty.X2, Dirty.Y2);

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

			printf("frames: needle %lu  value %lu  still %lu  |  last: draw %luus"
			       "  rect %lupx  work %luus  worst work %luus  \"%s\"\n",
			       (unsigned long)NeedleFrames, (unsigned long)ValueFrames,
			       (unsigned long)StillFrames, (unsigned long)DrawUs,
			       (unsigned long)PushPixels, (unsigned long)WorkUs,
			       (unsigned long)WorkMaxUs, Text);
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
