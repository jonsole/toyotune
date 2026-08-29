/*
 * clk.c
 *
 * Created: 07/09/2016 20:02:49
 *  Author: Jon
 */ 
#include "clk.h"
#include "pio.h"

void CLK_Init(void)
{
	/* Enable the 32.768Khz external crystal oscillator */
	OSC32KCTRL->XOSC32K.reg = OSC32KCTRL_XOSC32K_STARTUP(0x02) | OSC32KCTRL_XOSC32K_XTALEN |
							  OSC32KCTRL_XOSC32K_EN32K | OSC32KCTRL_XOSC32K_ENABLE;
	 
	/* Wait for the crystal oscillator to start up */
	while (~OSC32KCTRL->STATUS.reg & OSC32KCTRL_STATUS_XOSC32KRDY);
	 
	/* Configure the FDLL96MHz PLL to multiply by 1464.84 to provide a 48MHz clock */
	OSCCTRL->DPLLRATIO.reg = OSCCTRL_DPLLRATIO_LDR(1463) | OSCCTRL_DPLLRATIO_LDRFRAC(13);
	OSCCTRL->DPLLCTRLB.reg = OSCCTRL_DPLLCTRLB_LTIME(0) | OSCCTRL_DPLLCTRLB_REFCLK(0x00);
	OSCCTRL->DPLLPRESC.reg = 0;
	OSCCTRL->DPLLCTRLA.reg = OSCCTRL_DPLLCTRLA_ENABLE;
	 
	/* Wait and see if the DFLL output is good . . . */
	while (~OSCCTRL->DPLLSTATUS.reg & OSCCTRL_DPLLSTATUS_LOCK);
	 
	/* Set flash wait state to 1, which we need to do at 48MHz */
	NVMCTRL->CTRLB.reg |= NVMCTRL_CTRLB_RWS_DUAL;
	 
	/* For generic clock generator 0, select the DFLL48 Clock as input to generate 48MHz clock */
	GCLK->GENCTRL[0].reg = GCLK_GENCTRL_DIV(F_MCLK / F_GCLK0) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_DPLL96M | GCLK_GENCTRL_OE;;

	/* For generic clock generator 1, select the DFLL48 Clock as input to generate 1MHz clock */
	GCLK->GENCTRL[1].reg = GCLK_GENCTRL_DIV(F_MCLK / F_GCLK1) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_DPLL96M | GCLK_GENCTRL_OE;

#if 1
	/* Output 48Mhz clock from GCLK0 on PA27 */
	PIO_SetPeripheral(PIN_PA27, PIO_PERIPHERAL_H);
	PIO_EnablePeripheral(PIN_PA27);
#endif
}


void CLK_EnablePeripheral(uint8_t ClkGen, uint8_t PerCh)
{
 	GCLK->PCHCTRL[PerCh].reg &= ~GCLK_PCHCTRL_CHEN;
 	while (GCLK->PCHCTRL[PerCh].reg & GCLK_PCHCTRL_CHEN);
 	GCLK->PCHCTRL[PerCh].reg = (ClkGen << GCLK_PCHCTRL_GEN_Pos) | GCLK_PCHCTRL_CHEN;
 	while (!(GCLK->PCHCTRL[PerCh].reg & GCLK_PCHCTRL_CHEN));
}