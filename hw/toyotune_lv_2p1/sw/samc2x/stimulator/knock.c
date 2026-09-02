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
   6.7 kHz the original knock_6p7khz.raw was named for. 64 samples is a
   1.19 ms burst of eight cycles, decaying geometrically at 15/16 per sample.
   That is a damped resonance, which is what knock actually is, and it ends at
   mid-scale so the DAC rests where it started.

   Knock_Severity scales the burst: peak deviation from mid-scale is
   (Severity / 256) of half the DAC range, so 255 swings very nearly rail to
   rail and 64 gives about a quarter of that. With the board strapped to 5V,
   mid-scale is 2.5V.

   Triggering. Knock_Trigger() is called from TCC1_Handler in igt.c, on the
   MC0 capture - the falling edge of the ECU's own IGT output on PB08. So a
   burst is fired once per ignition event, referenced to the spark the ECU
   actually commanded rather than to crank angle, which is the right way round:
   real knock follows the spark. That also means no IGT wired to PB08 (EXT1
   pin 4) means no bursts at all.

   Two things to know before trusting what comes out of PA02:

     - igt.c calls Knock_Trigger(0). Severity 0 makes Mult 0, so every sample
       computes to 0 + 32768 - forty-eight beats of constant mid-scale. The
       envelope zeroes the whole waveform and no ping is produced. Pass a
       non-zero severity to hear anything; 64 gives about a quarter of full
       swing.

     - The idle level is not mid-scale. Knock_Init() ends with DATA = 0x200,
       which would be mid-scale for a right-adjusted 10-bit value, but CTRLB
       sets LEFTADJ - so the DAC takes bits 15:6 and the output is 8/1023 of
       full scale, essentially 0V. Combined with the point above, the pin sits
       near 0V until the first ignition and then steps to mid-scale and stays
       there, since DATA keeps whatever the last DMA beat wrote. Writing
       0x8000 would idle at mid-scale and remove the step.

   Also note the reference: CTRLB selects REFSEL_AVCC, so the 4.096V internal
   reference configured into SUPC->VREF just above it is not the one in use,
   and full scale follows AVCC instead. */



/* C:\Users\WinUser\Documents\toyotune-hw\ecu_stimulator\sw\samc2x\stimulator\knock_6p7khz.raw (11/02/2019 21:54:26)
   StartOffset(d): 00000000, EndOffset(d): 00000255, Length(d): 00000256 */

/* One cycle of sine as Q7, eight samples per cycle. At the 53.6 kHz sample
   rate that puts the tone at 53600/8 = 6700 Hz. */
static const int8_t KnockSine[8] = { 0, 90, 127, 90, 0, -90, -127, -90 };

/* 64 samples is 1.19 ms and eight cycles of ring-down. Kept short deliberately:
   at 7200 rpm - the top of the ECU's knock detection window - a four cylinder
   fires every 4.2 ms, so a longer burst would start running into the next
   ignition event. */
#define KNOCK_SAMPLES (64)

uint16_t KnockWaveform[KNOCK_SAMPLES];

void Knock_Trigger(uint8_t Severity)
{
	/* Peak deviation from mid-scale in DAC counts. Severity 255 is very nearly
	   full scale, 64 about a quarter of it. */
	int32_t Amplitude = (int32_t)Severity * 128;

	for (uint32_t Index = 0; Index < KNOCK_SAMPLES; Index++)
	{
		const int32_t Sample = (KnockSine[Index & 7] * Amplitude) / 127;
		KnockWaveform[Index] = (uint16_t)(32768 + Sample);

		/* Geometric decay, 15/16 per sample - a time constant of about 15
		   samples, so the ring is down to a few percent by the end of the
		   burst. Knock is a damped resonance and decays exponentially; the
		   previous linear ramp also stopped short, leaving the burst ending
		   abruptly at 6% of full amplitude rather than dying away. */
		Amplitude = (Amplitude * 15) / 16;
	}

	/* Finish exactly at mid-scale. The DAC holds whatever the last beat wrote,
	   so any residue here would sit as a DC offset until the next burst. */
	KnockWaveform[KNOCK_SAMPLES - 1] = 32768;

	/* Select channel and reset it */
	DMAC->CHID.reg = Knock_DmaChannel;
	DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

	/* Configure descriptor */
	DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(Knock_DmaChannel);
	DmaDesc->BTCTRL.reg = DMAC_BTCTRL_STEPSIZE_X1 | DMAC_BTCTRL_STEPSEL_SRC | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BEATSIZE_HWORD | 
						  DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_VALID;
	DmaDesc->BTCNT.reg = KNOCK_SAMPLES;
	DmaDesc->SRCADDR.reg = (uint32_t)&KnockWaveform[KNOCK_SAMPLES];
	DmaDesc->DSTADDR.reg = (uint32_t)&DAC->DATA;
	DmaDesc->DESCADDR.reg = 0;

	/* Enable DMA complete interrupt to stop TC0 */
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

	/* Enable TC0 Bus clock */
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC0;

	/* Enable 48MHz GCLK0 for TC0 */
	GCLK->PCHCTRL[TC0_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[TC0_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));

	/* Configure TC0 for 32KHz events */
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
