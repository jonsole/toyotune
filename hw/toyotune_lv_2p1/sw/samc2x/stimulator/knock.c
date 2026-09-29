/*
 * knock.c
 *
 * Created: 07/02/2019 18:57:51
 *  Author: WinUser
 */

#include <sam.h>

#include "debug.h"
#include "dmac.h"
#include "clk.h"
#include "pio.h"
#include "knock.h"

static uint8_t Knock_DmaChannel;

uint8_t Knock_Severity = 64;
uint8_t Knock_Noise = 16;



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
   6.7 kHz the original knock_6p7khz.raw was named for. The knock itself is
   64 samples, 1.19 ms, eight cycles decaying geometrically at 15/16 per
   sample: a damped resonance, which is what knock actually is.

   THE BACKGROUND. A real knock sensor is never silent: the engine vibrates
   all the time, and the knock chip judges the sensor on that background
   before it believes anything else. This used to output one burst per
   ignition and hold dead flat between them, and once the coolant was warm
   enough for the ECU to monitor knock at all, it concluded the sensor was
   disconnected - code 52, and its fail-safe maximum retard - at ANY burst
   severity, 64 included. So each ignition now starts a chain of two DMA
   descriptors: the knock burst with the background already mixed under it,
   then straight on into KNOCK_NOISE_SAMPLES of background alone - long enough
   to outlast the gap to the next ignition anywhere in the ECU's knock window.
   The output is never flat while the engine turns.

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

/* 64 samples is 1.19 ms and eight cycles of ring-down. Kept short deliberately:
   at 7200 rpm - the top of the ECU's knock detection window - a four cylinder
   fires every 4.2 ms, so a longer burst would start running into the next
   ignition event. */
#define KNOCK_SAMPLES (64)

/* 44.8 ms. At 700 rpm - the bottom of the knock window - a four cylinder fires
   every 42.9 ms, so the background outlasts the gap everywhere the ECU is
   listening. A whole number of carrier cycles, so it ends on a zero crossing. */
#define KNOCK_NOISE_SAMPLES (2400)

uint16_t KnockWaveform[KNOCK_SAMPLES];

static uint16_t KnockNoise[KNOCK_NOISE_SAMPLES];
static uint8_t KnockNoiseLevel;		/* Knock_Noise the buffer was built for */

/* The second link of the chain. Never written by the DMAC - its writeback goes
   to the channel's own entry - so it is set up once and reused. */
static DMAC_Descriptor_t KnockNoiseDesc __attribute__((aligned(16)));


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


void Knock_Trigger(uint8_t Severity)
{
	/* Picks up a Knock_Noise written over SWD. A one-off rebuild of the
	   buffer, which is also the only time the background can glitch. */
	if (Knock_Noise != KnockNoiseLevel)
		Knock_BuildNoise(Knock_Noise);

	/* Peak deviation from mid-scale in DAC counts. Severity 255 is very nearly
	   full scale, 64 about a quarter of it. */
	int32_t Amplitude = (int32_t)Severity * 128;

	for (uint32_t Index = 0; Index < KNOCK_SAMPLES; Index++)
	{
		/* The knock on top of the background, which the chain then carries
		   on with from sample KNOCK_SAMPLES - so there is no seam. */
		int32_t Sample = ((int32_t)KnockNoise[Index] - 32768)
		               + (KnockSine[Index & 7] * Amplitude) / 127;

		if (Sample > 32767)
			Sample = 32767;
		else if (Sample < -32768)
			Sample = -32768;

		KnockWaveform[Index] = (uint16_t)(32768 + Sample);

		/* Geometric decay, 15/16 per sample - a time constant of about 15
		   samples, so the ring is down to a few percent by the end of the
		   burst. Knock is a damped resonance and decays exponentially. */
		Amplitude = (Amplitude * 15) / 16;
	}

	/* Select channel and reset it */
	DMAC->CHID.reg = Knock_DmaChannel;
	DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

	/* The burst, then on into the background */
	DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(Knock_DmaChannel);
	DmaDesc->BTCTRL.reg = DMAC_BTCTRL_STEPSIZE_X1 | DMAC_BTCTRL_STEPSEL_SRC | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BEATSIZE_HWORD |
						  DMAC_BTCTRL_BLOCKACT_NOACT | DMAC_BTCTRL_VALID;
	DmaDesc->BTCNT.reg = KNOCK_SAMPLES;
	DmaDesc->SRCADDR.reg = (uint32_t)&KnockWaveform[KNOCK_SAMPLES];
	DmaDesc->DSTADDR.reg = (uint32_t)&DAC->DATA;
	DmaDesc->DESCADDR.reg = (uint32_t)&KnockNoiseDesc;

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

	/* The background, and the descriptor that plays it: everything after the
	   first KNOCK_SAMPLES, which the burst has already played with the knock
	   mixed in. */
	Knock_BuildNoise(Knock_Noise);

	KnockNoiseDesc.BTCTRL.reg = DMAC_BTCTRL_STEPSIZE_X1 | DMAC_BTCTRL_STEPSEL_SRC | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BEATSIZE_HWORD |
	                            DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_VALID;
	KnockNoiseDesc.BTCNT.reg = KNOCK_NOISE_SAMPLES - KNOCK_SAMPLES;
	KnockNoiseDesc.SRCADDR.reg = (uint32_t)&KnockNoise[KNOCK_NOISE_SAMPLES];
	KnockNoiseDesc.DSTADDR.reg = (uint32_t)&DAC->DATA;
	KnockNoiseDesc.DESCADDR.reg = 0;

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
