/*
 * test_dash.c
 *
 * Host tests for the parts of the dash node that do not need hardware:
 * frame decode, the signal store and its staleness, node identity, and page
 * selection.
 *
 * These are the pieces where a mistake is silent. A wrong byte offset or a
 * missed sign bit produces a gauge that reads plausibly and wrongly, which is
 * exactly the failure that survives a bench test - so the decode is asserted
 * against the same fixture the sending firmware's own packing test emits, and
 * the identity decode is swept across every possible ADC reading rather than
 * spot-checked at its nominal levels.
 *
 * Build and run:  python test/run_tests.py
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "node_id.h"
#include "pages.h"
#include "signal_store.h"
#include "signals.h"
#include "telemetry.h"

/* Defined in test_ui_model.c - the UI decisions are a separate suite
   because they exercise a different layer, but they share this runner. */
extern int UiTests_Run(int *Checks, int *Failures);

static int Failures = 0;
static int Checks = 0;

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


/***************************************************************************************/
/* Decode.
 *
 * The payloads below are the ones can_telemetry.c actually produced on the
 * bench for a plausible running engine - 3000 rpm, atmospheric, 4 ms of
 * injector, 82 C coolant, 14.6 V - taken from its own dump mode. Asserting
 * against the sender's real output rather than against bytes hand-written
 * here is what makes this a check of the two halves agreeing.
 */
static void TestDecodeFast(void)
{
	/* Rpm 3000 (0x0BB8), Tps 0x1234, Map 1013 (0x03F5), InjPw 4000 (0x0FA0) */
	static const uint8_t Fast[8] = { 0x0B, 0xB8, 0x12, 0x34, 0x03, 0xF5, 0x0F, 0xA0 };
	SignalReading_t R;

	printf("decode - FAST\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);

	CHECK(Telemetry_Handle(0x400, Fast, 8, 1000), "0x400 should be accepted");

	R = SignalStore_Get(SIGNAL_RPM, 1000);
	CHECK(R.Valid && R.Value == 3000, "Rpm should be 3000, got %ld", (long)R.Value);

	R = SignalStore_Get(SIGNAL_TPS_RAW, 1000);
	CHECK(R.Value == 0x1234, "TpsRaw big-endian, got 0x%04lX", (long)R.Value);

	R = SignalStore_Get(SIGNAL_MAP, 1000);
	CHECK(R.Value == 1013, "Map should be 101.3 kPa, got %ld", (long)R.Value);

	R = SignalStore_Get(SIGNAL_INJ_PW, 1000);
	CHECK(R.Value == 4000, "InjPw should be 4000 us, got %ld", (long)R.Value);
}


/* Signed signals are the ones a decoder gets wrong quietly: an unsigned read
   of a negative temperature gives a large positive number that still looks
   like a reading. */
static void TestDecodeSigned(void)
{
	/* Ect -1500 (0xFA24), Tha -400 (0xFE70), Tham 2070, Battery 1461 */
	static const uint8_t Med1[8] = { 0xFA, 0x24, 0xFE, 0x70, 0x08, 0x16, 0x05, 0xB5 };
	SignalReading_t R;

	printf("decode - MEDIUM1, signed temperatures\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);
	Telemetry_Handle(0x401, Med1, 8, 1000);

	R = SignalStore_Get(SIGNAL_ECT, 1000);
	CHECK(R.Value == -1500, "Ect should be -15.00 C, got %ld", (long)R.Value);

	R = SignalStore_Get(SIGNAL_THA, 1000);
	CHECK(R.Value == -400, "Tha should be -4.00 C, got %ld", (long)R.Value);

	R = SignalStore_Get(SIGNAL_THAM, 1000);
	CHECK(R.Value == 2070, "Tham should be 20.70 C, got %ld", (long)R.Value);

	R = SignalStore_Get(SIGNAL_BATTERY, 1000);
	CHECK(R.Value == 1461, "Battery should be 14.61 V, got %ld", (long)R.Value);
}


static void TestDecodeAddressing(void)
{
	static const uint8_t Fast[8] = { 0x0B, 0xB8, 0, 0, 0, 0, 0, 0 };

	printf("decode - identifier selection\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);

	/* The other board's block must be ignored entirely. Both publish the same
	   signal names, so decoding both would interleave two sources into one
	   gauge. */
	CHECK(!Telemetry_Handle(0x420, Fast, 8, 100), "CPU2's block is not ours");
	CHECK(!Telemetry_Handle(0x40A, Fast, 8, 100), "diag command is not telemetry");
	CHECK(!Telemetry_Handle(0x404, Fast, 8, 100), "RAW tier is deliberately not decoded");
	CHECK(!Telemetry_Handle(0x3FF, Fast, 8, 100), "below the block");
	CHECK(!SignalStore_Get(SIGNAL_RPM, 100).Valid, "nothing should have been stored");

	/* Switching base makes the same node listen to the other board. */
	Telemetry_Init(TELEMETRY_BASE_CPU2);
	CHECK(Telemetry_Handle(0x420, Fast, 8, 100), "CPU2's block after re-init");
	CHECK(SignalStore_Get(SIGNAL_RPM, 100).Value == 3000, "decoded from CPU2");
}


/* A short frame must be dropped, not padded: reading past the end would
   decode whatever the driver left in the buffer and look like a reading. */
static void TestDecodeShortFrame(void)
{
	static const uint8_t Short[4] = { 0x0B, 0xB8, 0x12, 0x34 };

	printf("decode - short frame\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);
	Telemetry_Handle(0x400, Short, 4, 100);

	CHECK(SignalStore_Get(SIGNAL_RPM, 100).Valid, "signals inside the frame decode");
	CHECK(!SignalStore_Get(SIGNAL_MAP, 100).Valid,
	      "Map is past the end and must not be invented");
	CHECK(!SignalStore_Get(SIGNAL_INJ_PW, 100).Valid, "nor InjPw");
}


static void TestProtocolVersion(void)
{
	uint8_t Info[8] = { DASH_EXPECTED_PROTOCOL_VERSION, 0, 1, 0, 0, 7, 0, 3 };

	printf("decode - protocol version\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);

	Telemetry_Handle(0x406, Info, 8, 100);
	CHECK(!Telemetry_ProtocolMismatch(), "matching version is not a mismatch");
	CHECK(SignalStore_Get(SIGNAL_TX_DROPPED, 100).Value == 7, "TxDropped");
	CHECK(SignalStore_Get(SIGNAL_BUS_OFF_RECOVERIES, 100).Value == 3, "BusOff");

	Info[0] = DASH_EXPECTED_PROTOCOL_VERSION + 1;
	Telemetry_Handle(0x406, Info, 8, 200);
	CHECK(Telemetry_ProtocolMismatch(), "an unknown version must be reported");
	CHECK(Telemetry_SeenProtocolVersion() == DASH_EXPECTED_PROTOCOL_VERSION + 1,
	      "and the version it saw recorded");
}


/***************************************************************************************/
static void TestStaleness(void)
{
	static const uint8_t Fast[8] = { 0x0B, 0xB8, 0, 0, 0, 0, 0, 0 };
	SignalReading_t R;

	printf("store - staleness\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);

	CHECK(!SignalStore_Get(SIGNAL_RPM, 0).Valid, "nothing is valid before a frame");
	CHECK(SignalStore_LinkAgeMs(0) == UINT32_MAX, "no link before the first frame");

	Telemetry_Handle(0x400, Fast, 8, 10000);

	R = SignalStore_Get(SIGNAL_RPM, 10000);
	CHECK(R.Valid && R.Fresh, "just-received is fresh");

	/* FAST is 20 ms, so three periods is 60 ms. */
	R = SignalStore_Get(SIGNAL_RPM, 10000 + 59);
	CHECK(R.Fresh, "still fresh just inside three periods");

	R = SignalStore_Get(SIGNAL_RPM, 10000 + 61);
	CHECK(!R.Fresh, "stale just outside three periods");
	CHECK(R.Valid, "but still valid - the last value is known, just old");

	/* Staleness must survive the millisecond counter wrapping, or a node that
	   has been powered for 49 days reports everything stale at once. */
	SignalStore_Init();
	SignalStore_Set(SIGNAL_RPM, 3000, 0xFFFFFFF0u);
	R = SignalStore_Get(SIGNAL_RPM, 0x00000005u);	/* 21 ms later, wrapped */
	CHECK(R.Fresh, "unsigned arithmetic must carry across the wrap");
}


static void TestLinkAlive(void)
{
	static const uint8_t Fast[8] = { 0x0B, 0xB8, 0, 0, 0, 0, 0, 0 };

	printf("store - link liveness\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);

	CHECK(!SignalStore_LinkAlive(0), "dead before anything arrives");
	Telemetry_Handle(0x400, Fast, 8, 5000);
	CHECK(SignalStore_LinkAlive(5000), "alive on arrival");
	CHECK(SignalStore_LinkAlive(5400), "still alive inside the window");
	CHECK(!SignalStore_LinkAlive(6000), "dead once the bus goes quiet");
}


/***************************************************************************************/
/* Node identity, swept across every reading the ADC can produce.
 *
 * The interesting property is not that the nominal levels decode - it is that
 * the gaps between them decode as UNKNOWN. Two nodes silently claiming the
 * same identity is a much worse failure than one node reporting that its
 * divider is wrong, so the decision must refuse rather than round.
 */
static void TestNodeId(void)
{
	uint32_t Raw;
	int Decoded[NODE_ID_COUNT];
	int Unknown = 0;
	uint8_t i;

	printf("node identity - full ADC sweep\n");
	memset(Decoded, 0, sizeof(Decoded));

	for (i = 0; i < NODE_ID_COUNT; i++)
	{
		uint16_t Nominal = (uint16_t)(((uint32_t)NodeId_NominalPermille[i] * 4095u) / 1000u);

		CHECK(NodeId_FromAdc(Nominal) == i,
		      "nominal level %u should decode to identity %u", Nominal, i);
	}

	for (Raw = 0; Raw <= 4095u; Raw++)
	{
		uint8_t Id = NodeId_FromAdc((uint16_t)Raw);

		if (Id == NODE_ID_UNKNOWN)
			Unknown++;
		else
			Decoded[Id]++;
	}

	CHECK(Unknown > 0, "there must be a dead band between levels");
	for (i = 0; i < NODE_ID_COUNT; i++)
		CHECK(Decoded[i] > 100, "identity %u should have a usable window, got %d",
		      i, Decoded[i]);

	/* The rails must not decode as an identity: a divider that has lost its
	   top or bottom resistor reads 0 or full scale, and that is a fault, not
	   node 0. */
	CHECK(NodeId_FromAdc(0) == NODE_ID_UNKNOWN, "a grounded pin is a fault");
	CHECK(NodeId_FromAdc(4095) == NODE_ID_UNKNOWN, "a pin at the rail is a fault");
}


/***************************************************************************************/
static void TestPages(void)
{
	uint8_t i;

	printf("pages - selection and startup\n");

	/* Every node starts somewhere sensible, and no two of the three share a
	   startup page - a cluster that powers on showing the same gauge three
	   times would be useless. */
	for (i = 0; i < 3; i++)
	{
		uint8_t j;

		Pages_Init(i, 0xFF);
		CHECK(Pages_Current() == StartupPage[i], "node %u startup page", i);

		for (j = 0; j < 3; j++)
			if (j != i)
				CHECK(StartupPage[i] != StartupPage[j],
				      "nodes %u and %u must not start on the same page", i, j);
	}

	/* An unreadable identity still has to start somewhere. */
	Pages_Init(NODE_ID_UNKNOWN, 0xFF);
	CHECK(Pages_Current() < PageCount, "unknown identity still starts on a page");

	/* A restored selection beats the startup page, or persistence is pointless. */
	Pages_Init(0, 2);
	CHECK(Pages_Current() == 2, "restored page wins");
	Pages_Init(0, 99);
	CHECK(Pages_Current() == StartupPage[0], "a corrupt restore falls back");
}


static void TestPageWrap(void)
{
	uint8_t Start, i;

	printf("pages - swiping wraps and skips the warning page\n");
	Pages_Init(0, 0);
	Start = Pages_Current();

	/* Swiping the whole way round must return to where it started, and must
	   never land on the warning page - that one is not reachable by choice. */
	for (i = 0; i < PageCount; i++)
	{
		Pages_Next();
		CHECK(strcmp(Pages[Pages_Current()].Name, "WARNING") != 0,
		      "swiping must not reach the warning page");
	}

	Pages_Init(0, 0);
	for (i = 0; i < PageCount - 1; i++)
		Pages_Next();
	CHECK(Pages_Current() == Start, "a full cycle returns to the start");

	Pages_Init(0, 0);
	Pages_Previous();
	Pages_Next();
	CHECK(Pages_Current() == Start, "previous then next is a no-op");
}


/* The fault takeover has to outrank the selection, and has to let go again. */
static void TestWarningTakeover(void)
{
	uint8_t Slow[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	printf("pages - fault takeover\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);
	Pages_Init(0, 0);

	Telemetry_Handle(0x403, Slow, 8, 1000);
	CHECK(!Pages_WarningActive(1000), "no flags set, no warning");
	CHECK(Pages_Effective(1000) == Pages_Current(), "selection is shown");

	Slow[3] = 0x04;		/* ErrorFlags1 */
	Telemetry_Handle(0x403, Slow, 8, 2000);
	CHECK(Pages_WarningActive(2000), "a fault flag raises the warning");
	CHECK(strcmp(Pages[Pages_Effective(2000)].Name, "WARNING") == 0,
	      "and the warning page is what gets drawn");

	/* A driver must not be able to swipe away from a fault. */
	Pages_Next();
	CHECK(strcmp(Pages[Pages_Effective(2000)].Name, "WARNING") == 0,
	      "swiping must not dismiss a fault");

	Slow[3] = 0;
	Telemetry_Handle(0x403, Slow, 8, 3000);
	CHECK(!Pages_WarningActive(3000), "the warning clears when the fault does");

	/* A stale flag must not hold the warning on: once the link drops, the
	   flag byte says nothing about the engine any more. SLOW is 500 ms, so
	   three periods is 1500 ms. */
	Slow[3] = 0x04;
	Telemetry_Handle(0x403, Slow, 8, 4000);
	CHECK(Pages_WarningActive(4000), "fresh fault warns");
	CHECK(!Pages_WarningActive(4000 + 1600), "a stale fault flag must not stick");
}


static void TestKnockThreshold(void)
{
	uint8_t Med2[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

	printf("pages - knock threshold\n");
	SignalStore_Init();
	Telemetry_Init(TELEMETRY_BASE_CPU1);
	Pages_Init(0, 0);

	/* Just under threshold: normal running, no takeover. */
	Med2[2] = (uint8_t)((PAGES_KNOCK_WARN_DEG100 - 100) >> 8);
	Med2[3] = (uint8_t)((PAGES_KNOCK_WARN_DEG100 - 100) & 0xFF);
	Telemetry_Handle(0x402, Med2, 8, 1000);
	CHECK(!Pages_WarningActive(1000), "knock below threshold does not warn");

	Med2[2] = (uint8_t)((PAGES_KNOCK_WARN_DEG100 + 100) >> 8);
	Med2[3] = (uint8_t)((PAGES_KNOCK_WARN_DEG100 + 100) & 0xFF);
	Telemetry_Handle(0x402, Med2, 8, 1100);
	CHECK(Pages_WarningActive(1100), "knock above threshold warns");
}


/***************************************************************************************/
static void TestFormatting(void)
{
	char Buf[24];

	printf("formatting\n");

	CHECK(strcmp(Signal_Format(SIGNAL_RPM, 3492, Buf, sizeof(Buf)), "3492") == 0,
	      "no decimals, got %s", Buf);
	CHECK(strcmp(Signal_Format(SIGNAL_ECT, 8179, Buf, sizeof(Buf)), "81.79") == 0,
	      "two decimals, got %s", Buf);
	CHECK(strcmp(Signal_Format(SIGNAL_MAP, 1013, Buf, sizeof(Buf)), "101.3") == 0,
	      "one decimal, got %s", Buf);

	/* The fraction must keep its leading zero, or 8.05 prints as 8.5. */
	CHECK(strcmp(Signal_Format(SIGNAL_ECT, 805, Buf, sizeof(Buf)), "8.05") == 0,
	      "leading zero in the fraction, got %s", Buf);

	/* Negative values must not come out as "-0.-2" from a truncating divide. */
	CHECK(strcmp(Signal_Format(SIGNAL_ECT, -1500, Buf, sizeof(Buf)), "-15.00") == 0,
	      "negative, got %s", Buf);
	CHECK(strcmp(Signal_Format(SIGNAL_MAP, -12, Buf, sizeof(Buf)), "-1.2") == 0,
	      "small negative, got %s", Buf);
}


/* Every signal must have a descriptor, or a gauge referencing it prints
   nothing and the omission is invisible until someone builds that page. */
static void TestDescriptorsComplete(void)
{
	int i;

	printf("descriptors\n");

	for (i = 0; i < SIGNAL_COUNT; i++)
	{
		CHECK(SignalDescriptors[i].Name != NULL && SignalDescriptors[i].Name[0],
		      "signal %d has no name", i);
		CHECK(SignalDescriptors[i].Unit != NULL, "signal %d has no unit", i);
		CHECK(SignalDescriptors[i].PeriodMs > 0, "signal %d has no period", i);
		CHECK(SignalDescriptors[i].Decimals <= 3, "signal %d decimals out of range", i);
	}
}


/* Every element on every page must reference a real signal. */
static void TestPagesReferenceRealSignals(void)
{
	uint8_t p, e;

	printf("pages - element references\n");

	for (p = 0; p < PageCount; p++)
	{
		CHECK(Pages[p].Name != NULL && Pages[p].Name[0], "page %u has no name", p);
		CHECK(Pages[p].ElementCount > 0, "page %u is empty", p);

		for (e = 0; e < Pages[p].ElementCount; e++)
		{
			const FaceElement_t *El = &Pages[p].Elements[e];

			CHECK(El->Signal < SIGNAL_COUNT,
			      "page %u element %u names signal %d", p, e, (int)El->Signal);
			CHECK(El->Max > El->Min, "page %u element %u has an empty range", p, e);
			CHECK((uint32_t)El->X + El->W <= 100 && (uint32_t)El->Y + El->H <= 100,
			      "page %u element %u runs off the panel", p, e);
		}
	}
}


/***************************************************************************************/
int main(void)
{
	printf("dash node tests\n");
	printf("---------------\n");

	TestDecodeFast();
	TestDecodeSigned();
	TestDecodeAddressing();
	TestDecodeShortFrame();
	TestProtocolVersion();
	TestStaleness();
	TestLinkAlive();
	TestNodeId();
	TestPages();
	TestPageWrap();
	TestWarningTakeover();
	TestKnockThreshold();
	TestFormatting();
	TestDescriptorsComplete();
	TestPagesReferenceRealSignals();
	UiTests_Run(&Checks, &Failures);

	printf("---------------\n");
	printf("%d checks, %d failures\n", Checks, Failures);
	return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
