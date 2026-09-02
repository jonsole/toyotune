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
	/* Enable the 32.768 khz external crystal oscillator */
	OSC32KCTRL->XOSC32K.reg = OSC32KCTRL_XOSC32K_STARTUP(0x06) | OSC32KCTRL_XOSC32K_XTALEN |
	OSC32KCTRL_XOSC32K_EN32K | OSC32KCTRL_XOSC32K_ENABLE;
	 
	/* Wait for the crystal oscillator to start up */
	while (~OSC32KCTRL->STATUS.reg & OSC32KCTRL_STATUS_XOSC32KRDY);
	 
	/* Configure the FDLL96MHz PLL to multiply by 1464.84 to provide a 48KHz clock */
	OSCCTRL->DPLLRATIO.reg = OSCCTRL_DPLLRATIO_LDR(1463) | OSCCTRL_DPLLRATIO_LDRFRAC(13);
	OSCCTRL->DPLLCTRLB.reg = OSCCTRL_DPLLCTRLB_LTIME(0) | OSCCTRL_DPLLCTRLB_REFCLK(0x00);
	OSCCTRL->DPLLPRESC.reg = 0;
	OSCCTRL->DPLLCTRLA.reg = OSCCTRL_DPLLCTRLA_ENABLE;
	 
	/* Wait and see if the DFLL output is good . . . */
	while (~OSCCTRL->DPLLSTATUS.reg & OSCCTRL_DPLLSTATUS_LOCK);
	 
	/* Set flash wait state to 2, which we need to do at 48MHz */
	NVMCTRL->CTRLB.reg |= NVMCTRL_CTRLB_RWS_DUAL;
	 
	/* For generic clock generator 0, select the DFLL96 Clock as input to generate 48MHz clock */
	GCLK->GENCTRL[0].reg = GCLK_GENCTRL_DIV(1) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_DPLL96M;

	/* For generic clock generator 1, select the DFLL96 Clock as input and divide by 48 to generate 1MHz clock */
	GCLK->GENCTRL[1].reg = GCLK_GENCTRL_DIV(48) | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_DPLL96M | GCLK_GENCTRL_OE;

	/* For generic clock generator 2, select the GCLK1 as input and divide by 64 = 2^(5+1) to generate 15.625KHz clock */
	GCLK->GENCTRL[2].reg = GCLK_GENCTRL_DIV(5) | GCLK_GENCTRL_DIVSEL | GCLK_GENCTRL_GENEN | GCLK_GENCTRL_SRC_GCLKGEN1 | GCLK_GENCTRL_OE;

	/* Output 1Mhz clock from GCLK1 on PB15 */
	PIO_SetPeripheral(PIN_PB15, PIO_PERIPHERAL_H);
	PIO_EnablePeripheral(PIN_PB15);
 }
