/*
 * igt.c
 *
 * Created: 09/02/2019 12:13:44
 *  Author: WinUser
 */ 

#include <sam.h>

#include "debug.h"
#include "clk.h"
#include "pio.h"
#include "igt.h"
#include "knock.h"

uint32_t IGT_CaptureTime;
volatile uint32_t IGT_TimingPeriod = 0;	/* Period in uS from falling edge of IGT to 12.5 ATDC */

void TCC1_Handler(void) __attribute__ ((__interrupt__));
void TCC1_Handler(void)
{
	uint32_t Status = TCC1->INTFLAG.reg;

	/* Falling edge of IGT on EXTINT8/PB08 */
	if (Status & TCC_INTFLAG_MC0)
	{
		IGT_CaptureTime = TCC1->CC[0].reg;
		Knock_Trigger(Knock_Severity);
	}
	
	/* Falling edge at 12.5 ATDC */
	if (Status & TCC_INTFLAG_MC1)
		IGT_TimingPeriod = TCC1->CC[1].reg - IGT_CaptureTime;
}

uint32_t IGT_GetTimingPeriod(void)
{
	return IGT_TimingPeriod;
}

void IGT_ClearTimingPeriod(void)
{
	IGT_TimingPeriod = 0;
}

void IGT_Init(void)
{
	/* Initialize GPIO */
	PIO_SetPeripheral(PIN_PB08, PIO_PERIPHERAL_A);
	PIO_EnablePeripheral(PIN_PB08);
	PIO_SetPeripheral(PIN_PB09, PIO_PERIPHERAL_A);
	PIO_EnablePeripheral(PIN_PB09);

	/* Enable EIC Bus clock */
	MCLK->APBAMASK.reg |= MCLK_APBAMASK_EIC;

	/* Enable 48MHz GCLK0 for EIC */
	GCLK->PCHCTRL[EIC_GCLK_ID].reg &= ~GCLK_PCHCTRL_CHEN;
	while (GCLK->PCHCTRL[EIC_GCLK_ID].reg & GCLK_PCHCTRL_CHEN);
	GCLK->PCHCTRL[EIC_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK0;
	GCLK->PCHCTRL[EIC_GCLK_ID].reg |= GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[EIC_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));

	/* Configure EIC for event on falling edge on EXTINT8 & EXTINT9 */
	EIC->CTRLA.reg = 0x00;															// Use GCLK_EIC
	EIC->CONFIG[0].reg = 0x00;														// EIC0-7
	EIC->CONFIG[1].reg = EIC_CONFIG_SENSE0_FALL | EIC_CONFIG_SENSE1_FALL;			// EIC8-15
	EIC->EVCTRL.reg = EIC_EVCTRL_EXTINTEO(1 << 8) | EIC_EVCTRL_EXTINTEO(1 << 9);	// Generate events for EXTINT8 & EXTINT9

	#if 0
	/* Enable interrupt for EXTINT9 */
	EIC->INTENSET.reg = EIC_INTENSET_EXTINT(1 << 9);
	NVIC_SetPriority(EIC_IRQn, 1);
	NVIC_EnableIRQ(EIC_IRQn);
	#endif

	/* Enable EIC */
	EIC->CTRLA.reg |= EIC_CTRLA_ENABLE;

	/* Enable Event System Bus clock */
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_EVSYS;

	/* Enable 48MHz GCLK0 for EVSYS channel 0 */
	GCLK->PCHCTRL[EVSYS_GCLK_ID_0].reg &= ~GCLK_PCHCTRL_CHEN;
	while (GCLK->PCHCTRL[EVSYS_GCLK_ID_0].reg & GCLK_PCHCTRL_CHEN);
	GCLK->PCHCTRL[EVSYS_GCLK_ID_0].reg = GCLK_PCHCTRL_GEN_GCLK0;
	GCLK->PCHCTRL[EVSYS_GCLK_ID_0].reg |= GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[EVSYS_GCLK_ID_0].reg & GCLK_PCHCTRL_CHEN));

	/* Configure channel 0 for user TCC1, generator EXTINT8, asynchronous */
	EVSYS->USER[EVSYS_ID_USER_TCC1_MC_0].reg = EVSYS_USER_CHANNEL(0 + 1);
	EVSYS->CHANNEL[0].reg = EVSYS_CHANNEL_EDGSEL_NO_EVT_OUTPUT | EVSYS_CHANNEL_PATH_ASYNCHRONOUS | EVSYS_CHANNEL_EVGEN(EVSYS_ID_GEN_EIC_EXTINT_8);

	/* Enable 48MHz GCLK0 for EVSYS channel 1 */
	GCLK->PCHCTRL[EVSYS_GCLK_ID_1].reg &= ~GCLK_PCHCTRL_CHEN;
	while (GCLK->PCHCTRL[EVSYS_GCLK_ID_1].reg & GCLK_PCHCTRL_CHEN);
	GCLK->PCHCTRL[EVSYS_GCLK_ID_1].reg = GCLK_PCHCTRL_GEN_GCLK0;
	GCLK->PCHCTRL[EVSYS_GCLK_ID_1].reg |= GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[EVSYS_GCLK_ID_1].reg & GCLK_PCHCTRL_CHEN));

	/* Configure channel 1 for user TCC1, generator EXTINT9, asynchronous */
	EVSYS->USER[EVSYS_ID_USER_TCC1_MC_1].reg = EVSYS_USER_CHANNEL(1 + 1);
	EVSYS->CHANNEL[1].reg = EVSYS_CHANNEL_EDGSEL_NO_EVT_OUTPUT | EVSYS_CHANNEL_PATH_ASYNCHRONOUS | EVSYS_CHANNEL_EVGEN(EVSYS_ID_GEN_EIC_EXTINT_9);

	#if 0
	/* Enable interrupt for channel 1 */
	EVSYS->INTENSET.reg = EVSYS_INTENSET_EVD1;
	NVIC_SetPriority(EVSYS_IRQn, 1);
	NVIC_EnableIRQ(EVSYS_IRQn);
	#endif

	/* Enable TCC1 Bus clock (Timer counter clock) */
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_TCC1;

	/* Enable 1MHz GCLK1 for TCC1 (timer counter input clock) */
	GCLK->PCHCTRL[TCC1_GCLK_ID].reg &= ~GCLK_PCHCTRL_CHEN;
	while (GCLK->PCHCTRL[TCC1_GCLK_ID].reg & GCLK_PCHCTRL_CHEN);
	GCLK->PCHCTRL[TCC1_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK1;
	GCLK->PCHCTRL[TCC1_GCLK_ID].reg |= GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[TCC1_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));

	/* Initialize TCC1 */
	TCC1->CTRLA.reg = TCC_CTRLA_CPTEN0 | TCC_CTRLA_CPTEN1;
	TCC1->PER.reg = TCC_PER_PER(0xFFFFFF);
	TCC1->EVCTRL.reg = TCC_EVCTRL_MCEI0 | TCC_EVCTRL_MCEI1;

	/* Enable TCC1 capture interrupts */
	TCC1->INTENSET.reg = TCC_INTENSET_MC0 | TCC_INTENSET_MC1;
	NVIC_SetPriority(TCC1_IRQn, 1);
	NVIC_EnableIRQ(TCC1_IRQn);

	/* Enable TCC1 */
	TCC1->CTRLA.reg |= TCC_CTRLA_ENABLE;
}

