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
 * WHAT IS NOT HERE YET
 *
 * The panel. It needs the vendor CO5300 QSPI driver and LVGL, and the board
 * has not arrived - writing a display layer against a datasheet and no
 * hardware would be guesswork. Everything below it is real and tested on a
 * host: signals, decode, the store, node identity and the page tables.
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


/***************************************************************************************/
/* Core 1: the display. A placeholder until the panel driver exists - it
   reports what would be drawn over USB serial, which is enough to prove the
   store, the page tables and the cross-core handover before any glass is
   attached. */
static void Core1Main(void)
{
	uint8_t LastPage = 0xFF;

	for (;;)
	{
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());
		uint8_t Page = Pages_Effective(NowMs);
		const FacePage_t *Face = &Pages[Page];
		char Text[24];
		uint8_t i;

		if (Page != LastPage)
		{
			printf("\n-- page %u: %s%s --\n", Page, Face->Name,
			       Pages_WarningActive(NowMs) ? "  (FAULT TAKEOVER)" : "");
			LastPage = Page;
		}

		if (!SignalStore_LinkAlive(NowMs))
		{
			printf("  no telemetry for %lums\n",
			       (unsigned long)SignalStore_LinkAgeMs(NowMs));
		}
		else if (Telemetry_ProtocolMismatch())
		{
			/* Deliberately loud, and deliberately not a gauge: the layout may
			   have moved, so any value drawn from it could be wrong. */
			printf("  PROTOCOL v%u, expected v%u - refusing to decode\n",
			       Telemetry_SeenProtocolVersion(), DASH_EXPECTED_PROTOCOL_VERSION);
		}
		else
		{
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

		sleep_ms(500);
	}
}


/***************************************************************************************/
int main(void)
{
	uint8_t Id;

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

	multicore_launch_core1(Core1Main);

	for (;;)
	{
#if DASH_HAVE_CAN2040
		CanLink_Poll(to_ms_since_boot(get_absolute_time()));
#endif
		tight_loop_contents();
	}
}
