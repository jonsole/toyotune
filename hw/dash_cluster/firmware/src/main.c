/*
 * main.c
 *
 * Dash node entry point and the two-core split.
 *
 *   Core 0   can2040 and its PIO interrupt, frame decode, the signal store
 *   Core 1   the panel and LVGL
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
 * The panel and LVGL live in panel.c and ui_lvgl.c. When the build cannot find
 * an LVGL checkout, core 1 reports what it would have drawn over USB serial
 * instead - which is enough to exercise the store, the page tables and the
 * cross-core handover with no glass attached.
 */

#include <stdio.h>

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

#if DASH_HAVE_LVGL
#include "panel.h"
#include "ui_lvgl.h"
#endif

/* How often the console status line goes out. Long, because it is a
   background reassurance rather than a data feed - the glass is the data
   feed. */
#define STATUS_PERIOD_MS	(2000u)

/* How often the gauges are re-read from the signal store. Matched to the
   fastest telemetry tier (20 ms, PLAN.md section 3): asking more often than
   the data can change only burns core 1. */
#define UI_UPDATE_PERIOD_MS	(20u)

/* Latched in main() and read by core 1.
 *
 * USB CDC discards everything printed before a host attaches, so the boot
 * banner is invisible to anyone who connects afterwards - which, on a board
 * that powers up with the ignition, is everyone. Repeating the identity in the
 * periodic status line is what makes it observable at all. */
static uint8_t DashNodeId;

#if DASH_HAVE_LVGL
/* CORE 1'S STACK, AND WHY IT IS NOT THE SDK'S.
 *
 * The SDK reserves core 1's stack in SCRATCH_X, which on RP2350 is a single
 * 4 KB bank - and its default is 2 KB of that. LVGL's software renderer
 * recurses through the widget tree and nests its blend and mask paths, and the
 * vendor panel driver builds a per-row fill buffer on the stack as well, so
 * 2 KB is not enough. A stack that overruns there does not report itself; it
 * appears as a hard fault somewhere inside a draw, which is a thoroughly
 * miserable thing to chase on a bench.
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


#if DASH_HAVE_LVGL
/***************************************************************************************/
/* Core 1: the display.
 *
 * Two cadences, deliberately separate. UiLvgl_Update() re-reads the signal
 * store and pushes values into the widget tree; LVGL's own timer handler
 * decides when any of that actually reaches the glass. Driving them from one
 * loop period would tie the redraw rate to the data rate, and the redraw is
 * the expensive half. */
static void Core1Main(void)
{
	uint32_t NextUiMs = 0;
	uint32_t NextStatusMs = 0;

	Panel_Init();
	UiLvgl_Init();

	for (;;)
	{
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());
		uint32_t WaitMs;

		if ((int32_t)(NowMs - NextUiMs) >= 0)
		{
			NextUiMs = NowMs + UI_UPDATE_PERIOD_MS;
			UiLvgl_Update(NowMs);
		}

		/* Returns how long it is content to be left alone, or
		   LV_NO_TIMER_READY when nothing at all is pending. */
		WaitMs = Panel_Service();

		if ((int32_t)(NowMs - NextStatusMs) >= 0)
		{
			uint16_t TouchX;
			uint16_t TouchY;

			NextStatusMs = NowMs + STATUS_PERIOD_MS;
			Panel_TouchLast(&TouchX, &TouchY);

			printf("node %u  page %u  %s  flush %lu  "
			       "touch %s rep %lu press %lu @%u,%u",
			       DashNodeId, Pages_Effective(NowMs),
			       SignalStore_LinkAlive(NowMs) ? "link up" : "LINK DOWN",
			       (unsigned long)Panel_Flushes(),
			       Panel_TouchPresent() ? "ok" : "ABSENT",
			       (unsigned long)Panel_TouchReports(),
			       (unsigned long)Panel_TouchPresses(), TouchX, TouchY);
			printf("  int %lu up / %lu down",
			       (unsigned long)Panel_TouchRiseEdges(),
			       (unsigned long)Panel_TouchFallEdges());
			printf("\n  refresh %lu  last %lums/%lupx  worst %lums",
			       (unsigned long)Panel_Refreshes(),
			       (unsigned long)Panel_RefreshLastMs(),
			       (unsigned long)Panel_RefreshLastPx(),
			       (unsigned long)Panel_RefreshMaxMs());
			printf("\n  bus %lu.%lu MB/s  %luus/frame  drain<=%lu",
			       (unsigned long)(Panel_FlushMbPerSx10() / 10u),
			       (unsigned long)(Panel_FlushMbPerSx10() % 10u),
			       (unsigned long)Panel_FlushBusyUsPerFrame(),
			       (unsigned long)Panel_DrainSpinsMax());
			if (Panel_FlushTimeouts() != 0u)
				printf("  flush-timeout %lu",
				       (unsigned long)Panel_FlushTimeouts());
			printf("\n");

			Core1ReportFace(NowMs, Pages_Effective(NowMs), false);
		}

		/* Capped so the update and status cadences above are still met when
		   LVGL has nothing to do. */
		if (WaitMs > UI_UPDATE_PERIOD_MS)
			WaitMs = UI_UPDATE_PERIOD_MS;
		sleep_ms(WaitMs);
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

#if DASH_HAVE_LVGL
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

#if DASH_HAVE_CAN2040
	CanLink_Init(Id);
#else
	printf("built without can2040 - see README; no telemetry will arrive\n");
#endif

#if DASH_HAVE_LVGL
	multicore_launch_core1_with_stack(Core1Main, Core1Stack,
	                                  sizeof(Core1Stack));
#else
	multicore_launch_core1(Core1Main);
#endif

	for (;;)
	{
#if DASH_HAVE_CAN2040
		CanLink_Poll(to_ms_since_boot(get_absolute_time()));
#endif
		tight_loop_contents();
	}
}
