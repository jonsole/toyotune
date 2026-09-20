/*
 * splash_seq.h
 *
 * The power-on splash as a timeline: given how long the splash has been
 * running, what is on the glass and how bright. No panel, no images - so the
 * sequence can be checked on a host, and changed in one place.
 *
 *     emblem fades in, holds, fades out
 *     this node's letter fades in, holds, fades out     (M, R or 2)
 *     done - the gauges take over
 *
 * A node with no letter - a fourth identity - skips straight from the emblem
 * to the gauges.
 */
#ifndef SPLASH_SEQ_H_
#define SPLASH_SEQ_H_

#include <stdbool.h>
#include <stdint.h>

#define SPLASH_EMBLEM_IN_MS	(1200u)
#define SPLASH_EMBLEM_HOLD_MS	(1000u)
#define SPLASH_EMBLEM_OUT_MS	(400u)
#define SPLASH_LETTER_IN_MS	(600u)
#define SPLASH_LETTER_HOLD_MS	(1200u)
#define SPLASH_LETTER_OUT_MS	(400u)

/* Full brightness. A level scales every palette entry by Level / SPLASH_FULL. */
#define SPLASH_FULL		(256u)

typedef enum
{
	SPLASH_SHOW_EMBLEM,
	SPLASH_SHOW_LETTER,
	SPLASH_SHOW_DONE
} SplashShow_t;

typedef struct
{
	SplashShow_t Show;
	uint32_t Level;		/* 0..SPLASH_FULL */
} SplashFrame_t;

/* What to show ElapsedMs into the splash. */
extern SplashFrame_t Splash_At(uint32_t ElapsedMs, bool HasLetter);

/* How long the whole thing lasts, for a node with a letter or without. */
extern uint32_t Splash_TotalMs(bool HasLetter);

#endif /* SPLASH_SEQ_H_ */
