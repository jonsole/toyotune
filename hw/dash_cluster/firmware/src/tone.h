/*
 * tone.h
 *
 * The warning sounds, as numbers.
 *
 * A warning is a rhythm before it is a pitch: the driver is looking at the
 * road, so what tells them WHICH warning is sounding is the pattern of pips,
 * not the note. So each warning gets its own (frequency, duration) sequence,
 * and the host tests refuse two warnings that share one - a second sound
 * identical to the first would be worse than no second sound at all, because
 * it would be acted on as the first.
 *
 * Everything here is integer arithmetic over a sine table and a phase
 * accumulator, with no dependency on the codec or on the PIO, so the patterns
 * and the envelope are testable on a host. src/audio.c is the half that has
 * to talk to hardware; it does nothing but hand buffers to Tone_Fill().
 *
 * THE ENVELOPE IS NOT DECORATION. A pip that starts and stops at full
 * amplitude puts a step into the speaker, and a step is a click - on a small
 * speaker in a hard-surfaced cabin, a loud one. Worse, it sounds like a fault
 * rather than a warning about one. So every amplitude change is slewed over a
 * couple of milliseconds, which also means a warning can be switched or
 * cancelled at any instant without a click.
 */

#ifndef TONE_H_
#define TONE_H_

#include <stdbool.h>
#include <stdint.h>

/* The codec's sample rate. 24 kHz with a 6.144 MHz master clock is the one
   combination Waveshare's own example runs on this board, and the ES8311's
   coefficient table has an entry for it - see audio.c. Nothing about a beep
   needs more. */
#define TONE_SAMPLE_RATE	(24000u)

/* Peak sample value: just short of full scale, so the note is as loud as it
   can be generated without clipping. It started at 12000, about a third of
   full scale, and was too quiet in practice - a warning nobody hears is not
   one. Loudness beyond this belongs to the codec volume, and only down: above
   0 dB there it is digital gain that would clip this into a rasp. */
#define TONE_PEAK		(30000)

/* The envelope's full amplitude, and how fast it moves - one step per sample,
   so 256/6 is about 1.8 ms from silence to full at 24 kHz. */
#define TONE_AMP_ONE		(256)
#define TONE_AMP_SLEW		(6)

/* One warning sound each. The order is the priority order: when two
   conditions stand at once the lower number is what the driver hears, so the
   sound they act on is the one that matters most. See warn.h. */
typedef enum
{
	TONE_NONE = 0,		/* silence - not a pattern */
	TONE_FAULT,		/* two-tone alternating, continuous */
	TONE_REV,		/* three fast high pips */
	TONE_BOOST,		/* two mid pips */
	TONE_MIXTURE,		/* one long low note */
	TONE_COUNT
} ToneId_t;

/* A step of a pattern: a note, or silence when Hz is zero. A pattern loops
   for as long as the warning stands. */
typedef struct
{
	uint16_t Hz;
	uint16_t Ms;
} ToneStep_t;

typedef struct
{
	ToneId_t Id;
	uint8_t Step;		/* where in the pattern */
	uint32_t Left;		/* samples left of this step */
	uint32_t Phase;		/* Q32 around the sine table */
	uint32_t Delta;		/* phase per sample, so the note */
	bool Silent;		/* this step is a gap, not a note */
	int32_t Amp;		/* 0..TONE_AMP_ONE, slewed towards the target */
} Tone_t;

/* Fills the sine table. Call once before any Tone_Fill(). */
extern void Tone_Init(void);

extern void Tone_Reset(Tone_t *T);

/* Choose what is sounding. Setting the pattern already playing is a no-op, so
   this can be called every time round a loop: a warning that stands keeps its
   rhythm rather than restarting from the first pip. */
extern void Tone_Set(Tone_t *T, ToneId_t Id);

/* Count stereo frames of 16-bit samples, in the I2S word layout the codec
   expects: the sample in the top half, which is the left channel and the one
   the speaker is on. Silence is exact zeros, not a held level. */
extern void Tone_Fill(Tone_t *T, int32_t *Out, uint32_t Count);

/* For the tests and the console: the pattern itself, and its length. */
extern const ToneStep_t *Tone_Pattern(ToneId_t Id, uint32_t *Steps);
extern const char *Tone_Name(ToneId_t Id);

#endif /* TONE_H_ */
