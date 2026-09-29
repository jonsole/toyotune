/*
 * knock.c
 *
 * Created: 07/02/2019 18:57:51
 *  Author: WinUser
 */

#include <sam.h>
#include <stdbool.h>

#include "debug.h"
#include "dmac.h"
#include "clk.h"
#include "pio.h"
#include "knock.h"
#include "vrg.h"

static uint8_t Knock_DmaChannel;

uint8_t Knock_Severity = 64;
uint8_t Knock_Noise = 16;
uint8_t Knock_DelayDeg = 25;
uint16_t Knock_RingSamples = 160;
uint8_t Knock_Decay = 252;
uint8_t Knock_Count = 4;
uint8_t Knock_Every = 20;

static uint8_t Knock_Ignition;	/* Position in the Knock_Every cycle */



/* Knock Signal Generator

   Uses DAC to generator knock signal.  DMA triggered by TC0 copies samples into DAC.DATA register

   Output pin and where it appears on the SAM C21 Xplained Pro:

     Signal          MCU pin   Xplained Pro
     Knock (DAC0)    PA02      DAC-OUT header pin 1, and EXT3 pin 15 (SPI_SS_A)

   DAC-OUT is the dedicated 2-pin header labelled on the silkscreen; pin 2 of
   it is ground. PA03 next door is the ADC/DAC voltage reference on the VREF
   header - do not confuse the two.

   Unlike NE/G1/G2 this is a single-ended analogue output, not a summed pair,
   so it needs no resistor network of its own. What it does need is whatever
   attenuation the ECU's knock input expects: a real piezo knock sensor
   delivers millivolts, and this drives 0 to AVCC.

   The waveform. TC0 raises an event every 48000000/53600 ticks of GCLK0, so
   samples leave at 53.6 kHz, and eight samples per cycle puts the tone at
   53600/8 = 6700 Hz - a plausible knock frequency for this bore, and the
   6.7 kHz the original knock_6p7khz.raw was named for. The knock itself is a
   damped resonance, which is what knock actually is: Knock_RingSamples of it
   (default 160, 3 ms), decaying at 63/64 per sample.

   WHEN IT LANDS. Not at the spark: real knock comes after TDC, when cylinder
   pressure peaks, and the knock chip listens in a window there. The ring
   starts Knock_DelayDeg of crank after the IGT edge (default 25, about 15
   degrees ATDC), converted to time at VRG_Rpm on every ignition so it tracks
   engine speed. It used to fire at the spark and be gone in a quarter of a
   millisecond - too early and too short for the chip to see.

   THE BACKGROUND. A real knock sensor is never silent: the engine vibrates
   all the time, and the knock chip judges the sensor on that background
   before it believes anything else. This used to output one burst per
   ignition and hold dead flat between them, and once the coolant was warm
   enough for the ECU to monitor knock at all, it concluded the sensor was
   disconnected - code 52, and its fail-safe maximum retard - at ANY burst
   severity, 64 included. So each ignition now starts a chain of three DMA
   descriptors: background for the delay, the ring with the background mixed
   under it, then the rest of KNOCK_NOISE_SAMPLES of background - long enough
   to outlast the gap to the next ignition anywhere in the ECU's knock window.
   With Knock_Noise above zero the output is never flat while the engine
   turns. (A background of 16 did not clear code 52 either, so the bench runs
   it at 0 while that is chased.)

   The background is the same 6.7 kHz carrier with a random amplitude each
   cycle, between half and all of Knock_Noise: roughly what engine vibration
   looks like after the knock chip's own band-pass. It is built once, and
   rebuilt at the next ignition if Knock_Noise has been changed.

   Knock_Severity and Knock_Noise are on the same scale: peak deviation from
   mid-scale is (value / 256) of half the DAC range, so 255 swings very nearly
   rail to rail and 64 gives about a quarter of that. With the board strapped
   to 5V, mid-scale is 2.5V.

   Triggering. Knock_Trigger() is called from TCC1_Handler in igt.c, on the
   MC0 capture - the falling edge of the ECU's own IGT output on PB08. So a
   burst is fired once per ignition event, referenced to the spark the ECU
   actually commanded rather than to crank angle, which is the right way round:
   real knock follows the spark. That also means no IGT wired to PB08 (EXT1
   pin 4) means no bursts, and no background either.

   Before the first ignition the DAC idles at mid-scale (DATA = 0x8000 with
   LEFTADJ). After the last one the background runs out and the DAC holds the
   final sample, which is set to mid-scale for that reason.

   Also note the reference: CTRLB selects REFSEL_AVCC, so the 4.096V internal
   reference configured into SUPC->VREF just above it is not the one in use,
   and full scale follows AVCC instead. */



/* One cycle of sine as Q7, eight samples per cycle. At the 53.6 kHz sample
   rate that puts the tone at 53600/8 = 6700 Hz. */
static const int8_t KnockSine[8] = { 0, 90, 127, 90, 0, -90, -127, -90 };

/* Samples per second, and so per ms: 53.6. */
#define KNOCK_SAMPLE_RATE (53600u)

/* The longest ring Knock_RingSamples may ask for: 9.6 ms, far past the gap
   between ignitions at any speed worth testing. */
#define KNOCK_RING_MAX (512)

/* 44.8 ms. At 700 rpm - the bottom of the knock window - a four cylinder fires
   every 42.9 ms, so the background outlasts the gap everywhere the ECU is
   listening. A whole number of carrier cycles, so it ends on a zero crossing. */
#define KNOCK_NOISE_SAMPLES (2400)

/* Where the delay link's background ends: one carrier cycle short of the end
   of KnockNoise, so the delay always finishes on a whole cycle and joins the
   ring - which starts at phase zero - without a step. It also caps the delay
   at 44.6 ms, far beyond Knock_DelayDeg at any speed in the knock window. */
#define KNOCK_DELAY_END (KNOCK_NOISE_SAMPLES - 8)

uint16_t KnockWaveform[KNOCK_RING_MAX];

static uint16_t KnockNoise[KNOCK_NOISE_SAMPLES];
static uint8_t KnockNoiseLevel;		/* Knock_Noise the buffer was built for */

/* The settings KnockWaveform was built for. It is rebuilt only when one of
   them changes, never per ignition - see Knock_BuildRing. */
static uint8_t KnockRingSeverity;
static uint8_t KnockRingDecay;
static uint16_t KnockRingLength;
static uint8_t KnockRingNoiseLevel;
static bool KnockRingValid;

/* The chain, three links: background for the delay (the channel's base
   descriptor), then the ring, then background to the end. Nothing is computed
   per ignition beyond the delay and the three descriptors, which matters
   because this runs in the IGT interrupt: building the ring there took about
   a millisecond, long enough to hold off the crank pattern's interrupt for
   two or three slots - the ECU saw the rpm wander - and to push every burst
   about 18 degrees later than Knock_DelayDeg asked for at 3000 rpm. The DMAC
   never writes these two (its writeback goes to the channel's own entry), and
   the channel is reset before they are changed. */
static DMAC_Descriptor_t KnockRingDesc __attribute__((aligned(16)));
static DMAC_Descriptor_t KnockTailDesc __attribute__((aligned(16)));


/* xorshift32: plenty for a vibration envelope, and no divider needed. */
static uint32_t Knock_RandomState = 0x2545F491u;

static uint32_t Knock_Random(void)
{
	uint32_t X = Knock_RandomState;

	X ^= X << 13;
	X ^= X >> 17;
	X ^= X << 5;
	Knock_RandomState = X;
	return X;
}


static void Knock_BuildNoise(uint8_t Level)
{
	int32_t Amplitude = 0;

	for (uint32_t Index = 0; Index < KNOCK_NOISE_SAMPLES; Index++)
	{
		/* A new amplitude every cycle, at the zero crossing so there is no
		   step: half to all of Level, on the same scale as Knock_Severity. */
		if ((Index & 7) == 0)
			Amplitude = ((int32_t)Level * 128 * (int32_t)(64 + (Knock_Random() & 63))) / 128;

		KnockNoise[Index] = (uint16_t)(32768 + (KnockSine[Index & 7] * Amplitude) / 127);
	}

	/* The DAC holds the last beat once the chain runs out - after the engine
	   stops - so leave it at mid-scale rather than part way up a cycle. */
	KnockNoise[KNOCK_NOISE_SAMPLES - 1] = 32768;

	KnockNoiseLevel = Level;
}


/* The ring: the knock over the first Ring samples of the background, so the
   chain runs on into the rest of it (the tail link) with no seam. Built over
   a fixed stretch of background rather than wherever the delay happens to
   end, which is what lets it be built once instead of per ignition. The
   delay link plays a different stretch, ending at KNOCK_DELAY_END; the
   background is random from cycle to cycle anyway, so the join cannot be
   told apart from any other cycle boundary. */
static void Knock_BuildRing(uint8_t Severity, uint32_t Ring, uint8_t Decay)
{
	/* Peak deviation from mid-scale in DAC counts. Severity 255 is very nearly
	   full scale, 64 about a quarter of it. */
	int32_t Amplitude = (int32_t)Severity * 128;

	for (uint32_t Index = 0; Index < Ring; Index++)
	{
		int32_t Sample = ((int32_t)KnockNoise[Index] - 32768)
		               + (KnockSine[Index & 7] * Amplitude) / 127;

		if (Sample > 32767)
			Sample = 32767;
		else if (Sample < -32768)
			Sample = -32768;

		KnockWaveform[Index] = (uint16_t)(32768 + Sample);

		/* Geometric decay, Knock_Decay/256 per sample. The default 252 is a
		   time constant of about 64 samples, 1.2 ms, so 160 samples (3 ms)
		   ring down to under a tenth; 254 doubles it, 255 quadruples it.
		   Knock is a damped resonance and decays exponentially; the old
		   15/16 was gone in a quarter of a millisecond. */
		Amplitude = (Amplitude * (int32_t)Decay) / 256;
	}

	KnockRingSeverity = Severity;
	KnockRingDecay = Decay;
	KnockRingLength = (uint16_t)Ring;
	KnockRingNoiseLevel = KnockNoiseLevel;
	KnockRingValid = true;
}


void Knock_Trigger(uint8_t Severity)
{
	/* Stop the previous chain before touching anything it reads. The DAC holds
	   its last sample until the new chain starts - microseconds. */
	DMAC->CHID.reg = Knock_DmaChannel;
	DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

	/* Picks up a Knock_Noise written over SWD. A one-off rebuild of the
	   buffer, which is also the only time the background can glitch. */
	if (Knock_Noise != KnockNoiseLevel)
		Knock_BuildNoise(Knock_Noise);

	uint32_t Ring = Knock_RingSamples;
	if (Ring < 1)
		Ring = 1;
	else if (Ring > KNOCK_RING_MAX)
		Ring = KNOCK_RING_MAX;

	/* Knock_DelayDeg of crank at the current speed, in samples: degrees over
	   (rpm * 6 degrees per second), times the sample rate. At least one
	   sample, since a DMA block cannot be empty, and no longer than the
	   stretch of background the delay link plays. One software divide - the
	   only arithmetic left in here that scales with nothing. */
	const uint32_t Rpm = VRG_GetRpm();
	uint32_t Delay = (Rpm > 0)
	               ? ((uint32_t)Knock_DelayDeg * KNOCK_SAMPLE_RATE) / (6u * Rpm)
	               : 1u;
	if (Delay < 1)
		Delay = 1;
	else if (Delay > KNOCK_DELAY_END)
		Delay = KNOCK_DELAY_END;

	/* Picks up any setting written over SWD. A one-off rebuild, which is the
	   only time this interrupt runs long - so poke settings between tests,
	   not while measuring. */
	if (!KnockRingValid || Severity != KnockRingSeverity || Knock_Decay != KnockRingDecay ||
	    Ring != KnockRingLength || KnockNoiseLevel != KnockRingNoiseLevel)
		Knock_BuildRing(Severity, Ring, Knock_Decay);

	/* Knock on the first Knock_Count of every Knock_Every ignitions, and play
	   the background alone on the rest. A knock chip judges knock against a
	   background level it keeps learning, so the same burst on every ignition
	   just becomes the background, however big: severity 255 on every spark
	   gave a knock level of 0. The quiet ignitions play the same stretch of
	   background the ring was built over, with no knock added. */
	bool Knocks = true;
	if (Knock_Every > 1)
	{
		if (++Knock_Ignition >= Knock_Every)
			Knock_Ignition = 0;
		if (Knock_Ignition >= Knock_Count)
			Knocks = false;
	}

	/* The ring, then the rest of the background */
	KnockRingDesc.BTCTRL.reg = DMAC_BTCTRL_STEPSIZE_X1 | DMAC_BTCTRL_STEPSEL_SRC | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BEATSIZE_HWORD |
	                           DMAC_BTCTRL_BLOCKACT_NOACT | DMAC_BTCTRL_VALID;
	KnockRingDesc.BTCNT.reg = (uint16_t)Ring;
	KnockRingDesc.SRCADDR.reg = Knocks ? (uint32_t)&KnockWaveform[Ring] : (uint32_t)&KnockNoise[Ring];
	KnockRingDesc.DSTADDR.reg = (uint32_t)&DAC->DATA;
	KnockRingDesc.DESCADDR.reg = (uint32_t)&KnockTailDesc;

	KnockTailDesc.BTCNT.reg = (uint16_t)(KNOCK_NOISE_SAMPLES - Ring);

	/* The delay: background ending on a whole cycle at KNOCK_DELAY_END, then
	   on into the ring. (DMAC source addresses are the END of the block.) */
	DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(Knock_DmaChannel);
	DmaDesc->BTCTRL.reg = DMAC_BTCTRL_STEPSIZE_X1 | DMAC_BTCTRL_STEPSEL_SRC | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BEATSIZE_HWORD |
						  DMAC_BTCTRL_BLOCKACT_NOACT | DMAC_BTCTRL_VALID;
	DmaDesc->BTCNT.reg = (uint16_t)Delay;
	DmaDesc->SRCADDR.reg = (uint32_t)&KnockNoise[KNOCK_DELAY_END];
	DmaDesc->DSTADDR.reg = (uint32_t)&DAC->DATA;
	DmaDesc->DESCADDR.reg = (uint32_t)&KnockRingDesc;

	/* Enable DMA complete interrupt */
	DMAC->CHINTENSET.reg = DMAC_CHINTENSET_MASK;

	/* Configure channel to start transfer on TC0 MC0 event */
	DMAC->CHCTRLB.reg = DMAC_CHCTRLB_TRIGACT_BEAT | DMAC_CHCTRLB_TRIGSRC(0x1C);
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;

	/* Turn on TC0 to start regular DMA transfers */
	TC0->COUNT16.CTRLA.reg |= TC_CTRLA_ENABLE;
}

void Knock_Interrupt(void *Data, const uint8_t Channel, uint16_t IntPending)
{
	const uint8_t IntStatus = DMAC->CHINTFLAG.reg;

	/* Clear all channel interrupts */
	DMAC->CHINTFLAG.reg = IntStatus;
}

void Knock_Init(void)
{
	/* Initialize GPIO for analog functions */
	PIO_SetPeripheral(PIN_PA02, PIO_PERIPHERAL_B);
	PIO_EnablePeripheral(PIN_PA02);

	/* Allocate DMA channels for DAC output */
	Knock_DmaChannel = DMAC_ChannelAllocate(Knock_Interrupt, NULL);

	/* The background, and the tail link that plays out the rest of it after
	   the ring. Only its length changes per ignition, set in Knock_Trigger;
	   its end address is always the end of the buffer. */
	Knock_BuildNoise(Knock_Noise);

	KnockTailDesc.BTCTRL.reg = DMAC_BTCTRL_STEPSIZE_X1 | DMAC_BTCTRL_STEPSEL_SRC | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BEATSIZE_HWORD |
	                           DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_VALID;
	KnockTailDesc.BTCNT.reg = 1;
	KnockTailDesc.SRCADDR.reg = (uint32_t)&KnockNoise[KNOCK_NOISE_SAMPLES];
	KnockTailDesc.DSTADDR.reg = (uint32_t)&DAC->DATA;
	KnockTailDesc.DESCADDR.reg = 0;

	/* Enable TC0 Bus clock */
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC0;

	/* Enable 48MHz GCLK0 for TC0 */
	GCLK->PCHCTRL[TC0_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[TC0_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));

	/* Configure TC0 for 53.6 kHz events */
	TC0->COUNT16.CTRLA.reg = TC_CTRLA_MODE_COUNT16 | TC_CTRLA_PRESCALER_DIV1 | TC_CTRLA_PRESCSYNC_RESYNC;
	TC0->COUNT16.WAVE.reg = TC_WAVE_WAVEGEN_MFRQ;
	TC0->COUNT16.EVCTRL.reg = TC_EVCTRL_MCEO0;
	TC0->COUNT16.CC[0].reg = (48000000UL / 53600UL) - 1;
	TC0->COUNT16.COUNT.reg = 0;

	/* Configure VREF voltage */
	SUPC->VREF.reg = SUPC_VREF_SEL_4V096 | SUPC_VREF_ONDEMAND;

	/* Enable DAC Bus clock */
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_DAC;

	/* Enable 48MHz GCLK0 for DAC */
	GCLK->PCHCTRL[DAC_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK1 | GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[DAC_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));

	/* Reset DAC */
	DAC->CTRLA.reg = DAC_CTRLA_SWRST;
	while (DAC->SYNCBUSY.reg & DAC_SYNCBUSY_SWRST);

	/* Configure and enable DAC */
	DAC->CTRLB.reg = DAC_CTRLB_REFSEL_AVCC | DAC_CTRLB_EOEN | DAC_CTRLB_LEFTADJ;
	DAC->EVCTRL.reg = DAC_EVCTRL_EMPTYEO;
	DAC->CTRLA.reg = DAC_CTRLA_ENABLE;
	DAC->DATA.reg = 0x8000;
}
