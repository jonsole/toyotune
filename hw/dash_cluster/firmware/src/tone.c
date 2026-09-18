/*
 * tone.c - the warning sounds. See tone.h.
 */

#include "tone.h"

#include <math.h>
#include <string.h>


/* --- the patterns --------------------------------------------------------
 *
 * Chosen to be told apart with the eyes on the road, which means separating
 * them in RHYTHM first and pitch second:
 *
 *   fault    a continuous two-tone that never pauses - the only one with no
 *            gap in it, so it reads as "stop" rather than "look"
 *   rev      three fast pips, the highest note; the one that must cut through
 *            an engine at 7000 rpm
 *   boost    two slower pips, a mid note
 *   mixture  one long low note, slowly repeated - a nag rather than an alarm,
 *            which is right: a lean reading wants the throttle backed off,
 *            not a flinch
 */
static const ToneStep_t FaultPattern[] =
{
	{ 880u, 250u }, { 1175u, 250u }
};

static const ToneStep_t RevPattern[] =
{
	{ 2600u, 60u }, { 0u, 60u }, { 2600u, 60u }, { 0u, 60u },
	{ 2600u, 60u }, { 0u, 400u }
};

static const ToneStep_t BoostPattern[] =
{
	{ 1800u, 120u }, { 0u, 100u }, { 1800u, 120u }, { 0u, 600u }
};

static const ToneStep_t MixturePattern[] =
{
	{ 700u, 400u }, { 0u, 900u }
};

static const struct
{
	const ToneStep_t *Steps;
	uint32_t Count;
	const char *Name;
} Patterns[TONE_COUNT] =
{
	[TONE_NONE]    = { NULL,           0u,                                 "quiet" },
	[TONE_FAULT]   = { FaultPattern,   sizeof(FaultPattern) / sizeof(FaultPattern[0]), "fault" },
	[TONE_REV]     = { RevPattern,     sizeof(RevPattern) / sizeof(RevPattern[0]), "rev" },
	[TONE_BOOST]   = { BoostPattern,   sizeof(BoostPattern) / sizeof(BoostPattern[0]), "boost" },
	[TONE_MIXTURE] = { MixturePattern, sizeof(MixturePattern) / sizeof(MixturePattern[0]), "mixture" }
};


/* 256 points of a sine, the whole table indexed by the top byte of the phase
   accumulator. Built at init rather than written out as a constant: the same
   three lines are easier to check than 256 numbers. */
#define TONE_TABLE	(256u)
static int16_t Sine[TONE_TABLE];


/***************************************************************************************/
void Tone_Init(void)
{
	uint32_t i;

	for (i = 0; i < TONE_TABLE; i++)
	{
		double A = (2.0 * 3.14159265358979323846 * (double)i) / (double)TONE_TABLE;

		Sine[i] = (int16_t)(sin(A) * (double)TONE_PEAK);
	}
}


/***************************************************************************************/
void Tone_Reset(Tone_t *T)
{
	memset(T, 0, sizeof(*T));
	T->Id = TONE_NONE;
}


/***************************************************************************************/
const ToneStep_t *Tone_Pattern(ToneId_t Id, uint32_t *Steps)
{
	if ((uint32_t)Id >= (uint32_t)TONE_COUNT)
		Id = TONE_NONE;
	if (Steps != NULL)
		*Steps = Patterns[Id].Count;
	return Patterns[Id].Steps;
}


/***************************************************************************************/
const char *Tone_Name(ToneId_t Id)
{
	if ((uint32_t)Id >= (uint32_t)TONE_COUNT)
		return "?";
	return Patterns[Id].Name;
}


/***************************************************************************************/
/* Phase increment for a note. Q32, so a frequency of Hz comes out exact to
   within a part in four billion and the table index is the top byte. */
static uint32_t Tone_Delta(uint32_t Hz)
{
	return (uint32_t)(((uint64_t)Hz << 32) / (uint64_t)TONE_SAMPLE_RATE);
}


/***************************************************************************************/
static void Tone_EnterStep(Tone_t *T, uint32_t Step)
{
	const ToneStep_t *P = Patterns[T->Id].Steps;
	uint32_t Count = Patterns[T->Id].Count;

	if (P == NULL || Count == 0u)
	{
		T->Left = 0u;
		T->Silent = true;
		return;
	}

	T->Step = (uint8_t)(Step % Count);
	T->Left = ((uint32_t)P[T->Step].Ms * TONE_SAMPLE_RATE) / 1000u;
	T->Silent = (P[T->Step].Hz == 0u);

	/* A silent step keeps the previous note's phase increment rather than
	   stopping the oscillator, so the couple of milliseconds of release ramp
	   fades the note out. Freezing the phase instead would fade a DC level,
	   which is a thump rather than a note ending. */
	if (!T->Silent)
		T->Delta = Tone_Delta(P[T->Step].Hz);
}


/***************************************************************************************/
void Tone_Set(Tone_t *T, ToneId_t Id)
{
	if ((uint32_t)Id >= (uint32_t)TONE_COUNT)
		Id = TONE_NONE;

	/* Already sounding: leave the rhythm alone. Restarting the pattern every
	   time the condition is re-checked would turn three pips into a stutter
	   that never gets past the first one. */
	if (Id == T->Id)
		return;

	T->Id = Id;
	T->Step = 0u;
	Tone_EnterStep(T, 0u);

	/* Amp is deliberately left where it was, so a switch between warnings -
	   or a cancellation - slews rather than steps. */
}


/***************************************************************************************/
void Tone_Fill(Tone_t *T, int32_t *Out, uint32_t Count)
{
	uint32_t i;

	for (i = 0; i < Count; i++)
	{
		int32_t Target;
		int32_t Sample;

		if (T->Left == 0u)
			Tone_EnterStep(T, (uint32_t)T->Step + 1u);

		if (T->Left > 0u)
			T->Left--;

		Target = (T->Id != TONE_NONE && !T->Silent) ? TONE_AMP_ONE : 0;

		if (T->Amp < Target)
		{
			T->Amp += TONE_AMP_SLEW;
			if (T->Amp > Target)
				T->Amp = Target;
		}
		else if (T->Amp > Target)
		{
			T->Amp -= TONE_AMP_SLEW;
			if (T->Amp < Target)
				T->Amp = Target;
		}

		/* Silence is exact zeros. A held non-zero level would be DC into the
		   speaker for as long as the gap lasts. */
		if (T->Amp == 0)
		{
			Out[i] = 0;
			T->Phase += T->Delta;
			continue;
		}

		Sample = ((int32_t)Sine[T->Phase >> 24] * T->Amp) / TONE_AMP_ONE;
		T->Phase += T->Delta;

		/* The codec takes 16 bits per channel in a 32-bit frame, left first.
		   Mono: the sample on the left, which is where the speaker is. */
		Out[i] = Sample << 16;
	}
}
