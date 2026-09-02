/*
 * sercom.c
 *
 * Created: 17/10/2016 20:28:38
 *  Author: Jon
 */ 
#include "sercom.h"
#include "pio.h"

void SERCOM_EnableClock(uint8_t Instance)
{
	static const uint8_t GclkPch[6] =     {SERCOM0_GCLK_ID_CORE, SERCOM1_GCLK_ID_CORE, SERCOM2_GCLK_ID_CORE, SERCOM3_GCLK_ID_CORE, SERCOM4_GCLK_ID_CORE, SERCOM5_GCLK_ID_CORE};
	static const uint8_t GclkPchSlow[6] = {SERCOM0_GCLK_ID_SLOW, SERCOM1_GCLK_ID_SLOW, SERCOM2_GCLK_ID_SLOW, SERCOM3_GCLK_ID_SLOW, SERCOM4_GCLK_ID_SLOW, SERCOM5_GCLK_ID_SLOW};

	MCLK->APBCMASK.reg |= (MCLK_APBCMASK_SERCOM0 << Instance);

	const uint8_t PchSlow = GclkPchSlow[Instance];
	GCLK->PCHCTRL[PchSlow].reg &= ~GCLK_PCHCTRL_CHEN;
	while (GCLK->PCHCTRL[PchSlow].reg & GCLK_PCHCTRL_CHEN);
	GCLK->PCHCTRL[PchSlow].reg = GCLK_PCHCTRL_GEN_GCLK0;
	GCLK->PCHCTRL[PchSlow].reg |= GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[PchSlow].reg & GCLK_PCHCTRL_CHEN));

	const uint8_t Pch = GclkPch[Instance];
	GCLK->PCHCTRL[Pch].reg &= ~GCLK_PCHCTRL_CHEN;
	while (GCLK->PCHCTRL[Pch].reg & GCLK_PCHCTRL_CHEN);
	GCLK->PCHCTRL[Pch].reg = GCLK_PCHCTRL_GEN_GCLK0;
	GCLK->PCHCTRL[Pch].reg |= GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[Pch].reg & GCLK_PCHCTRL_CHEN));
}


void SERCOM_UsartInit(uint8_t Instance, uint32_t CtrlA, uint32_t CtrlB, uint32_t CtrlC, uint16_t Baud)
{
	SercomUsart *Usart = &SERCOM_GetSercom(Instance)->USART;

	/* Reset USART */
	Usart->CTRLA.reg = SERCOM_USART_CTRLA_SWRST;

	/* Configure USART */
	SERCOM_UsartSyncWait(Usart);
	Usart->BAUD.reg = Baud;
	SERCOM_UsartSyncWait(Usart);
	Usart->CTRLC.reg = CtrlC;
	SERCOM_UsartSyncWait(Usart);
	Usart->CTRLB.reg = CtrlB;
	SERCOM_UsartSyncWait(Usart);
	Usart->CTRLA.reg = CtrlA;
	SERCOM_UsartSyncWait(Usart);	
}

void SERCOM_SpiInit(uint8_t Instance, uint32_t CtrlA, uint32_t CtrlB, uint16_t Baud)
{
	SercomSpi *Spi = &SERCOM_GetSercom(Instance)->SPI;

	/* Reset USART */
	Spi->CTRLA.reg = SERCOM_SPI_CTRLA_SWRST;

	/* Configure USART */
	SERCOM_SpiSyncWait(Spi);
	Spi->BAUD.reg = Baud;
	SERCOM_SpiSyncWait(Spi);
	Spi->CTRLB.reg = CtrlB;
	SERCOM_SpiSyncWait(Spi);
	Spi->CTRLA.reg = CtrlA;
	SERCOM_SpiSyncWait(Spi);
}

void SERCOM_ConfigurePios(uint8_t Instance)
{	switch (Instance)
	{
		/* SERCOM0 -> PA04,05,06 */
		case 0:
		{
			PIO_SetPeripheral(PIN_PA04, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PA04);
			PIO_SetPeripheral(PIN_PA05, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PA05);
			PIO_SetPeripheral(PIN_PA06, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PA06);
		}
		break;

		/* SERCOM1 -> PA16,17,18 */
		case 1:
		{
			PIO_SetPeripheral(PIN_PA16, PIO_PERIPHERAL_C);
			PIO_EnablePeripheral(PIN_PA16);
			PIO_SetPeripheral(PIN_PA17, PIO_PERIPHERAL_C);
			PIO_EnablePeripheral(PIN_PA17);
			PIO_SetPeripheral(PIN_PA18, PIO_PERIPHERAL_C);
			PIO_EnablePeripheral(PIN_PA18);
		}
		break;

		/* SERCOM2 -> PA08,09,10 */
		case 2:
		{
			PIO_SetPeripheral(PIN_PA08, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PA08);
			PIO_SetPeripheral(PIN_PA09, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PA09);
			PIO_SetPeripheral(PIN_PA10, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PA10);
		}
		break;

		/* SERCOM3 -> PA22,23,24 */
		case 3:
		{
			PIO_SetPeripheral(PIN_PA22, PIO_PERIPHERAL_C);
			PIO_EnablePeripheral(PIN_PA22);
			PIO_SetPeripheral(PIN_PA23, PIO_PERIPHERAL_C);
			PIO_EnablePeripheral(PIN_PA23);
			PIO_SetPeripheral(PIN_PA24, PIO_PERIPHERAL_C);
			PIO_EnablePeripheral(PIN_PA24);
		}
		break;

		/* SERCOM4 -> PB10 (Tx), PB11 (Rx), PB14 (Alternative Tx) */
		case 4:
		{
			PIO_SetPeripheral(PIN_PB10, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PB10);
			PIO_SetPeripheral(PIN_PB11, PIO_PERIPHERAL_D);
			PIO_EnablePeripheral(PIN_PB11);
			PIO_SetPeripheral(PIN_PB14, PIO_PERIPHERAL_C);
			PIO_EnablePeripheral(PIN_PB14);
		}
		break;

	}
}

 