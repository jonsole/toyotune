/*
 * test_warn.c - host tests for the warning decision and the sounds it picks.
 *
 * Two things here are invisible on a bench and expensive in a car. The first
 * is a threshold: nothing about a beep says whether it fired at 6800 rpm or
 * 68 rpm, so every condition is driven across its own boundary. The second is
 * the ownership rule - which node sounds what - which on a bench with one
 * board looks like it works however wrong it is, because there is no second
 * speaker to be silent.
 */

#include <stdio.h>
#include <string.h>

#include "pages.h"
#include "signal_store.h"
#include "tone.h"
#include "warn.h"

extern int WarnTests_Run(int *Checks, int *Failures);

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


/* The page showing each signal, found rather than assumed - the page list is
   reordered often enough that a hard-coded index here would be a test that
   passes while the firmware has gone silent. */
static int PageShowing(SignalId_t Signal)
{
	uint8_t p;
	uint32_t i;

	for (p = 0; p < PageCount; p++)
	{
		for (i = 0; i < Pages[p].ElementCount; i++)
		{
			if (Pages[p].Elements[i].Signal == Signal)
				return (int)p;
		}
	}
	return -1;
}


/* A clean store with the engine idling quietly and nothing wrong. */
static void Quiet(uint32_t NowMs)
{
	SignalStore_Init();
	SignalStore_Set(SIGNAL_RPM, 900, NowMs);
	SignalStore_Set(SIGNAL_MAP, WARN_ATMOSPHERE_KPA10 - 500, NowMs);
	SignalStore_Set(SIGNAL_AFR, 1450, NowMs);
}


/***************************************************************************************/
static void TestNothingWrong(void)
{
	Warn_t W;
	int RpmPage = PageShowing(SIGNAL_RPM);

	Warn_Init(&W);
	Quiet(1000u);
	CHECK(RpmPage >= 0, "some page shows the revs");
	CHECK(Warn_Update(&W, 1000u, (uint8_t)RpmPage, 0u) == WARN_NONE,
	      "an idling engine is silent");

	/* And an empty store - before any telemetry has arrived - says nothing
	   either. A node that beeps at power-on because it has no data would be
	   the first thing anyone disconnected. */
	SignalStore_Init();
	Warn_Init(&W);
	CHECK(Warn_Update(&W, 1000u, (uint8_t)RpmPage, 0u) == WARN_NONE,
	      "no data is not a warning");
}


/***************************************************************************************/
static void TestRevLimit(void)
{
	Warn_t W;
	uint32_t T = 1000u;
	int Page = PageShowing(SIGNAL_RPM);
	int Other = PageShowing(SIGNAL_AFR);

	Warn_Init(&W);
	Quiet(T);

	SignalStore_Set(SIGNAL_RPM, WARN_REV_ON_RPM - 1, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_NONE,
	      "one rpm below the threshold is not a warning");

	SignalStore_Set(SIGNAL_RPM, WARN_REV_ON_RPM, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_REV, "and on it, it is");

	/* Hysteresis: it holds on the way back down, so a needle hunting around
	   the threshold gives one tone rather than a machine-gun of them. */
	SignalStore_Set(SIGNAL_RPM, WARN_REV_ON_RPM - 100, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_REV,
	      "it holds between the two thresholds");
	CHECK(W.Fired[WARN_REV] == 1u, "and has still only fired once: %lu",
	      (unsigned long)W.Fired[WARN_REV]);

	SignalStore_Set(SIGNAL_RPM, WARN_REV_OFF_RPM - 1, T);
	(void)Warn_Update(&W, T, (uint8_t)Page, 0u);
	CHECK(!W.On[WARN_REV], "below the lower threshold it lets go");

	/* But it is still SOUNDING, because a condition that came and went in one
	   frame must still give a whole pip. */
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_REV,
	      "the hold keeps it audible");
	CHECK(Warn_Update(&W, T + WARN_HOLD_MS, (uint8_t)Page, 0u) == WARN_NONE,
	      "and it goes quiet when the hold expires");

	/* The hold must outlast the sounding part of the pattern, or a spike
	   would be cut off half way through and sound like a different warning. */
	{
		const ToneStep_t *P;
		uint32_t Steps, i, Sounding = 0u;

		P = Tone_Pattern(TONE_REV, &Steps);
		for (i = 0; i < Steps; i++)
		{
			if (P[i].Hz != 0u)
				Sounding = P[i].Ms + Sounding;
			else if (Sounding > 0u && i + 1u < Steps && P[i + 1u].Hz != 0u)
				Sounding += P[i].Ms;	/* a gap inside the burst */
		}
		CHECK(Sounding <= WARN_HOLD_MS,
		      "the hold covers the whole burst: %lu ms of pips, %lu ms held",
		      (unsigned long)Sounding, (unsigned long)WARN_HOLD_MS);
	}

	/* Ownership: the same condition, on a node showing something else. */
	Warn_Init(&W);
	SignalStore_Set(SIGNAL_RPM, WARN_REV_ON_RPM + 500, T);
	CHECK(Other >= 0 && Other != Page, "there is another page to be on");
	CHECK(Warn_Update(&W, T, (uint8_t)Other, 0u) == WARN_NONE,
	      "a node showing mixture does not announce the revs");
	CHECK(W.On[WARN_REV],
	      "but it tracked the condition anyway, so swiping to it beeps at once");
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 2u) == WARN_REV,
	      "and any node showing the revs announces them, identity aside");
}


/***************************************************************************************/
static void TestBoostAndMixture(void)
{
	Warn_t W;
	uint32_t T = 1000u;
	int Page = PageShowing(SIGNAL_MAP);
	int AfrPage = PageShowing(SIGNAL_AFR);

	Warn_Init(&W);
	Quiet(T);

	/* A safe mixture first. Quiet() leaves the engine at a perfectly normal
	   idle 14.5:1, and that same reading under a bar of boost is itself a
	   warning - which is the whole point of the mixture condition, and would
	   otherwise mask the boost one being tested here. */
	SignalStore_Set(SIGNAL_AFR, 1150, T);
	SignalStore_Set(SIGNAL_MAP, WARN_BOOST_ON_KPA10 - 1, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_NONE,
	      "just under the boost threshold is quiet");
	SignalStore_Set(SIGNAL_MAP, WARN_BOOST_ON_KPA10, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_BOOST, "and on it it sounds");

	/* And the mixture that was safe at idle, under boost, is not. */
	Warn_Init(&W);
	Quiet(T);
	SignalStore_Set(SIGNAL_MAP, WARN_BOOST_ON_KPA10 - 1, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_MIXTURE,
	      "an idle mixture at a bar of boost is a lean warning");

	/* Lean at cruise is the ECU doing its job. This is the one that would
	   have beeped down every motorway if the load gate were left out. */
	Warn_Init(&W);
	Quiet(T);
	SignalStore_Set(SIGNAL_AFR, 1600, T);		/* 16:1, part throttle */
	CHECK(Warn_Update(&W, T, (uint8_t)AfrPage, 0u) == WARN_NONE,
	      "16:1 off the throttle is not a warning");

	/* The same mixture with the engine loaded is a different matter. */
	SignalStore_Set(SIGNAL_MAP, WARN_LOAD_ON_KPA10, T);
	CHECK(Warn_Update(&W, T, (uint8_t)AfrPage, 0u) == WARN_MIXTURE,
	      "16:1 under load is");

	/* Rich is deliberately not warned about: it costs power, not pistons. */
	Warn_Init(&W);
	Quiet(T);
	SignalStore_Set(SIGNAL_MAP, WARN_LOAD_ON_KPA10, T);
	SignalStore_Set(SIGNAL_AFR, 1000, T);		/* 10:1, very rich */
	CHECK(Warn_Update(&W, T, (uint8_t)AfrPage, 0u) == WARN_NONE,
	      "rich under load is not sounded");
}


/***************************************************************************************/
static void TestStaleAndPriority(void)
{
	Warn_t W;
	uint32_t T = 1000u;
	int Page = PageShowing(SIGNAL_RPM);
	SignalReading_t R = { 0, 0u, false, false };
	uint32_t Late;

	/* A reading that has stopped arriving must not keep warning. Find a time
	   by which the store itself calls the value stale, rather than guessing
	   one - the freshness windows are per signal. */
	Warn_Init(&W);
	Quiet(T);
	SignalStore_Set(SIGNAL_RPM, WARN_REV_ON_RPM + 200, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_REV, "it is warning");

	for (Late = T; Late < T + 60000u; Late += 100u)
	{
		R = SignalStore_Get(SIGNAL_RPM, Late);
		if (!R.Fresh)
			break;
	}
	CHECK(!R.Fresh, "the store eventually calls the reading stale");
	(void)Warn_Update(&W, Late, (uint8_t)Page, 0u);
	CHECK(!W.On[WARN_REV], "a stale reading holds no condition");

	/* It does keep sounding for the rest of the hold, which is deliberate: a
	   link that drops mid-pip should finish the pip rather than cut it. */
	CHECK(Warn_Update(&W, Late + WARN_HOLD_MS, (uint8_t)Page, 0u) == WARN_NONE,
	      "and once the hold is out, a stale reading warns about nothing: %s",
	      Warn_Name(Warn_Update(&W, Late + WARN_HOLD_MS, (uint8_t)Page, 0u)));

	/* Priority. Two conditions at once give the more serious sound, and only
	   one - a driver cannot act on two at a time. */
	Warn_Init(&W);
	Quiet(T);
	SignalStore_Set(SIGNAL_RPM, WARN_REV_ON_RPM + 200, T);
	SignalStore_Set(SIGNAL_MAP, WARN_BOOST_ON_KPA10 + 200, T);
	CHECK(Warn_Update(&W, T, (uint8_t)Page, 0u) == WARN_REV,
	      "revs outrank boost on the tachometer page");
	CHECK(W.On[WARN_BOOST], "though both conditions are held");

	/* The fault takeover puts one page on every node, so identity decides -
	   otherwise three speakers would announce it three times over. */
	Warn_Init(&W);
	Quiet(T);
	SignalStore_Set(SIGNAL_ERROR_FLAGS1, 0x04, T);
	CHECK(Pages_WarningActive(T), "a stored fault takes the screen over");
	CHECK(Warn_Update(&W, T, Pages_Effective(T), WARN_SHARED_NODE) == WARN_FAULT,
	      "and the shared node sounds it");
	CHECK(Warn_Update(&W, T, Pages_Effective(T), WARN_SHARED_NODE + 1u) == WARN_NONE,
	      "while the others stay quiet");
}


/***************************************************************************************/
/* Every warning must be told apart by ear, so no two patterns may be the
   same, and each must map to a sound of its own. */
static void TestPatternsDiffer(void)
{
	uint32_t a, b;

	for (a = (uint32_t)WARN_NONE + 1u; a < (uint32_t)WARN_COUNT; a++)
	{
		ToneId_t Ta = Warn_Tone((WarnId_t)a);
		uint32_t StepsA;
		const ToneStep_t *Pa = Tone_Pattern(Ta, &StepsA);

		CHECK(Ta != TONE_NONE, "%s has a sound", Warn_Name((WarnId_t)a));
		CHECK(Pa != NULL && StepsA > 0u, "%s has a pattern",
		      Warn_Name((WarnId_t)a));

		for (b = a + 1u; b < (uint32_t)WARN_COUNT; b++)
		{
			ToneId_t Tb = Warn_Tone((WarnId_t)b);
			uint32_t StepsB;
			const ToneStep_t *Pb = Tone_Pattern(Tb, &StepsB);

			CHECK(Ta != Tb, "%s and %s do not share a sound",
			      Warn_Name((WarnId_t)a), Warn_Name((WarnId_t)b));
			CHECK(StepsA != StepsB
			      || memcmp(Pa, Pb, StepsA * sizeof(Pa[0])) != 0,
			      "%s and %s do not sound identical",
			      Warn_Name((WarnId_t)a), Warn_Name((WarnId_t)b));
		}
	}
}


/***************************************************************************************/
/* The synthesis itself: the right note, a clean start and exact silence. */
static void TestSynthesis(void)
{
	static int32_t Out[TONE_SAMPLE_RATE / 4u];	/* a quarter of a second */
	Tone_t T;
	uint32_t i, Crossings = 0u;
	int32_t Peak = 0;
	int32_t Hz;
	const ToneStep_t *P;
	uint32_t Steps;

	Tone_Init();
	Tone_Reset(&T);

	/* Silence really is silence - a held level would be DC into a speaker. */
	Tone_Fill(&T, Out, 64u);
	for (i = 0; i < 64u; i++)
	{
		if (Out[i] != 0)
			break;
	}
	CHECK(i == 64u, "nothing sounding gives exact zeros, first at %lu",
	      (unsigned long)i);

	/* The fault tone holds one note for 250 ms, which is long enough to count
	   its zero crossings. */
	P = Tone_Pattern(TONE_FAULT, &Steps);
	CHECK(Steps >= 1u && P[0].Hz > 0u, "the fault pattern starts on a note");

	Tone_Set(&T, TONE_FAULT);

	/* The attack ramps: the first samples must be small even though the note
	   is at full amplitude within a couple of milliseconds. */
	Tone_Fill(&T, Out, 4u);
	CHECK(Out[0] == 0, "the first sample of a beep is silence, not a step");

	Tone_Reset(&T);
	Tone_Set(&T, TONE_FAULT);
	Tone_Fill(&T, Out, (P[0].Ms * TONE_SAMPLE_RATE) / 1000u);

	for (i = 1u; i < (P[0].Ms * TONE_SAMPLE_RATE) / 1000u; i++)
	{
		int32_t A = Out[i - 1u] >> 16;
		int32_t B = Out[i] >> 16;

		if ((A < 0 && B >= 0) || (A >= 0 && B < 0))
			Crossings++;
		if (B > Peak)
			Peak = B;
	}

	/* Two crossings a cycle. */
	Hz = (int32_t)((Crossings * 1000u) / (2u * P[0].Ms));
	CHECK(Hz > (int32_t)P[0].Hz - 20 && Hz < (int32_t)P[0].Hz + 20,
	      "the note is %u Hz, measured %ld", P[0].Hz, (long)Hz);
	CHECK(Peak > (TONE_PEAK * 9) / 10 && Peak <= TONE_PEAK,
	      "and reaches full amplitude without clipping: %ld", (long)Peak);

	/* The gap after it is silent again, and the pattern loops rather than
	   stopping. */
	Tone_Fill(&T, Out, (P[1].Ms * TONE_SAMPLE_RATE) / 2000u);
	CHECK(T.Step == 1u, "the second step is next: %u", T.Step);

	/* Asking for the same sound again does not restart it. */
	{
		uint8_t Was = T.Step;
		uint32_t Left = T.Left;

		Tone_Set(&T, TONE_FAULT);
		CHECK(T.Step == Was && T.Left == Left,
		      "re-requesting the sound already playing leaves it alone");
	}
}


/***************************************************************************************/
int WarnTests_Run(int *OutChecks, int *OutFailures)
{
	Checks = 0;
	Failures = 0;

	printf("warnings - thresholds, which node sounds them, and the sounds\n");
	TestNothingWrong();
	TestRevLimit();
	TestBoostAndMixture();
	TestStaleAndPriority();
	TestPatternsDiffer();
	TestSynthesis();

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
