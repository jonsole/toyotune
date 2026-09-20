/*
 * test_splash.c - host tests for the power-on splash's timeline.
 *
 * What a bench cannot easily show is the edges: whether the emblem is really
 * black at the start, whether one image hands to the next without a flash of
 * full brightness, and whether a node with no letter ends on time.
 */

#include <stdio.h>

#include "splash_seq.h"

extern int SplashTests_Run(int *Checks, int *Failures);

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


/***************************************************************************************/
int SplashTests_Run(int *OutChecks, int *OutFailures)
{
	const uint32_t EmblemEnd = SPLASH_EMBLEM_IN_MS + SPLASH_EMBLEM_HOLD_MS
	                           + SPLASH_EMBLEM_OUT_MS;
	SplashFrame_t F;
	uint32_t t, Prev;

	Checks = 0;
	Failures = 0;
	printf("splash - the power-on sequence\n");

	F = Splash_At(0u, true);
	CHECK(F.Show == SPLASH_SHOW_EMBLEM && F.Level == 0u,
	      "it starts on the emblem, black: level %lu", (unsigned long)F.Level);

	F = Splash_At(SPLASH_EMBLEM_IN_MS, true);
	CHECK(F.Show == SPLASH_SHOW_EMBLEM && F.Level == SPLASH_FULL,
	      "full brightness once it has faded in");

	/* The fade only ever rises on the way in - no step back down. */
	for (t = 0, Prev = 0; t <= SPLASH_EMBLEM_IN_MS; t += 10u)
	{
		F = Splash_At(t, true);
		CHECK(F.Level >= Prev, "the fade in never dims, at %lu ms", (unsigned long)t);
		Prev = F.Level;
	}

	/* Half way through a fade looks like a quarter - the eye sees ratios. */
	F = Splash_At(SPLASH_EMBLEM_IN_MS / 2u, true);
	CHECK(F.Level > SPLASH_FULL / 5u && F.Level < SPLASH_FULL / 3u,
	      "half way in is about a quarter bright: %lu", (unsigned long)F.Level);

	/* The hand-over: the emblem ends dark, the letter starts dark. Anything
	   else is a flash on the glass between the two. */
	F = Splash_At(EmblemEnd - 1u, true);
	CHECK(F.Show == SPLASH_SHOW_EMBLEM && F.Level < SPLASH_FULL / 50u,
	      "the emblem's last frame is nearly black: %lu", (unsigned long)F.Level);
	F = Splash_At(EmblemEnd, true);
	CHECK(F.Show == SPLASH_SHOW_LETTER && F.Level == 0u,
	      "the letter's first frame is black: %lu", (unsigned long)F.Level);

	F = Splash_At(EmblemEnd + SPLASH_LETTER_IN_MS + 10u, true);
	CHECK(F.Show == SPLASH_SHOW_LETTER && F.Level == SPLASH_FULL, "the letter holds at full");

	F = Splash_At(Splash_TotalMs(true), true);
	CHECK(F.Show == SPLASH_SHOW_DONE, "and it is over at %lu ms",
	      (unsigned long)Splash_TotalMs(true));
	CHECK(Splash_TotalMs(true) <= 5000u, "the whole splash is under five seconds: %lu",
	      (unsigned long)Splash_TotalMs(true));

	/* A node with no letter goes from the emblem to its gauges. */
	F = Splash_At(EmblemEnd, false);
	CHECK(F.Show == SPLASH_SHOW_DONE, "no letter: done when the emblem is");
	CHECK(Splash_TotalMs(false) == EmblemEnd, "and the total says so");

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
