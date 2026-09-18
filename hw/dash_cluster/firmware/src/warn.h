/*
 * warn.h
 *
 * What is worth making a noise about, and which node makes it.
 *
 * WHICH NODE SOUNDS WHAT
 *
 * A node sounds a warning only when the face it is showing is a face that
 * displays the reading the warning is about: the rev warning on whichever
 * node is showing the tachometer, the boost and mixture warnings on whichever
 * is showing boost or AFR - the needle gauge or the trace, either will do.
 * That keeps sound and sight together, so the driver's eyes go to a gauge
 * that is already telling them the same thing, and it stops three speakers
 * announcing the same event three times a fraction apart.
 *
 * The exception is the fault takeover, which puts the same page on every node
 * at once (see Pages_Effective). There the page cannot pick a node, so node
 * WARN_SHARED_NODE sounds it and the others stay quiet. That is a guess about
 * a car that has not been wired yet - if the speaker ends up on a different
 * node, this is the one line to change.
 *
 * WHY THE CONDITIONS ARE NOT THE FACE'S RED BAND
 *
 * The red band painted on a dial is a presentation choice in pages.c; these
 * are thresholds for an action. They are close on purpose but must be able to
 * move apart: a band that starts at 1.0 bar looks right on a face, while the
 * tone wants a little more than that so a needle resting on the line does not
 * chirp. Each threshold has a hysteresis of its own for the same reason, and
 * a minimum hold so a momentary spike gives a whole pip rather than a click.
 *
 * A STALE READING NEVER WARNS. If the link drops, every condition here goes
 * quiet - a warning derived from a value that is no longer arriving is a
 * statement about the past presented as the present.
 */

#ifndef WARN_H_
#define WARN_H_

#include <stdbool.h>
#include <stdint.h>

#include "tone.h"

/* The warnings, in the order they outrank one another - the same order as
   ToneId_t, which they map to one for one. */
typedef enum
{
	WARN_NONE = 0,
	WARN_FAULT,		/* a stored fault, the limiter flags, or heavy knock */
	WARN_REV,		/* up against the limiter */
	WARN_BOOST,		/* past the boost the engine is mapped for */
	WARN_MIXTURE,		/* lean, under load */
	WARN_COUNT
} WarnId_t;

/* Which node sounds a warning that every node is showing - see above. */
#define WARN_SHARED_NODE	(0u)

/* --- the thresholds ------------------------------------------------------ */

/* The 3S-GTE's limiter cuts around 7000 rpm and the tachometer's red band
   starts there, so the tone comes in just below it: by the time it sounds the
   driver is already out of usable revs. */
#define WARN_REV_ON_RPM		(6800)
#define WARN_REV_OFF_RPM	(6500)

/* Boost, in tenths of a kPa ABSOLUTE, as the MAP signal carries it. Just past
   the top of the face's red band, which starts at 1.0 bar of gauge pressure.
   pages.c defines the atmosphere it is measured against; repeated here rather
   than shared, because these are a decision about the engine and those are a
   decision about a drawing. */
#define WARN_ATMOSPHERE_KPA10	(1013)
#define WARN_BOOST_ON_KPA10	(WARN_ATMOSPHERE_KPA10 + 1050)		/* 1.05 bar */
#define WARN_BOOST_OFF_KPA10	(WARN_ATMOSPHERE_KPA10 + 950)		/* 0.95 bar */

/* Mixture, in hundredths of an AFR. Lean only: too rich costs power and
   washes the bores, and is not what a tone at 100 mph is for. And lean only
   UNDER LOAD - a cruise at 16:1 is the ECU doing its job, so an unconditional
   window would beep down every motorway. */
#define WARN_AFR_LEAN_ON_X100	(1300)		/* leaner than 13.0:1 */
#define WARN_AFR_LEAN_OFF_X100	(1270)
#define WARN_LOAD_ON_KPA10	(WARN_ATMOSPHERE_KPA10 + 300)		/* 0.30 bar */
#define WARN_LOAD_OFF_KPA10	(WARN_ATMOSPHERE_KPA10 + 200)

/* How long a warning is held once it has started, so a spike that crosses a
   threshold for one frame still gives a recognisable pip rather than a tick. */
#define WARN_HOLD_MS		(400u)

typedef struct
{
	bool On[WARN_COUNT];		/* the condition, after hysteresis */
	uint32_t HoldMs[WARN_COUNT];	/* earliest it may go quiet */
	uint32_t Fired[WARN_COUNT];	/* how many times each has started */
	bool Load;			/* the mixture warning's load gate */
} Warn_t;

extern void Warn_Init(Warn_t *W);

/* Look at the store and decide. Page is the face this node is actually
   showing (Pages_Effective), NodeId its identity. Returns the one warning to
   sound, or WARN_NONE.

   Every condition is evaluated whatever page is up, so the hysteresis and the
   hold track the engine rather than the driver's swiping; the page only
   decides whether this node is the one that says so. */
extern WarnId_t Warn_Update(Warn_t *W, uint32_t NowMs, uint8_t Page, uint8_t NodeId);

/* The sound for a warning. One to one, but named rather than cast, so the two
   enumerations can be reordered independently. */
extern ToneId_t Warn_Tone(WarnId_t Id);

extern const char *Warn_Name(WarnId_t Id);

#endif /* WARN_H_ */
