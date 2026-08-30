/*
 * sercom.c
 *
 * Created: 17/10/2016 20:28:38
 *  Author: Jon
 */ 
#include "sercom.h"
#include "pio.h"
#include "clk.h"

void SERCOM_EnableClock(uint8_t Instance)
{
	const uint8_t GclkPch[6] =     {19, 20, 21, 22, 23, 25};
	const uint8_t GclkPchSlow[6] = {18, 18, 18, 18, 18, 24};

	MCLK->APBCMASK.reg |= (MCLK_APBCMASK_SERCOM0 << Instance);

	/* Enable 48Mhz GCLK0 for SERCOM instance */
	CLK_EnablePeripheral(0, GclkPchSlow[Instance]);
	CLK_EnablePeripheral(0, GclkPch[Instance]);
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

void SERCOM_ConfigurePios(uint8_t Instance, uint8_t Mask)
{	
	switch (Instance)
	{
		/* SERCOM0 -> PA04,05,06 */
		case 0:
		{
			if (Mask & 0x01)
			{	
				PIO_SetPeripheral(PIN_PA04, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PA04);
			}
			if (Mask & 0x02)
			{
				PIO_SetPeripheral(PIN_PA05, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PA05);
			}
			if (Mask & 0x04)
			{
				PIO_SetPeripheral(PIN_PA06, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PA06);
			}
		}
		break;

		/* SERCOM1 -> PA16,17,18 */
		case 1:
		{
			if (Mask & 0x01)
			{
				PIO_SetPeripheral(PIN_PA16, PIO_PERIPHERAL_C);
				PIO_EnablePeripheral(PIN_PA16);
			}
			if (Mask & 0x02)
			{
				PIO_SetPeripheral(PIN_PA17, PIO_PERIPHERAL_C);
				PIO_EnablePeripheral(PIN_PA17);
			}
			if (Mask & 0x04)
			{
				PIO_SetPeripheral(PIN_PA18, PIO_PERIPHERAL_C);
				PIO_EnablePeripheral(PIN_PA18);
			}
		}
		break;

		/* SERCOM2 -> PA08,09,10 */
		case 2:
		{
			if (Mask & 0x01)
			{
				PIO_SetPeripheral(PIN_PA08, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PA08);
			}
			if (Mask & 0x02)
			{				
				PIO_SetPeripheral(PIN_PA09, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PA09);
			}
			if (Mask & 0x04)
			{
				PIO_SetPeripheral(PIN_PA10, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PA10);
			}
		}
		break;

		/* SERCOM3 -> PA22,23,24 */
		case 3:
		{
			if (Mask & 0x01)
			{
				PIO_SetPeripheral(PIN_PA22, PIO_PERIPHERAL_C);
				PIO_EnablePeripheral(PIN_PA22);
			}
			if (Mask & 0x02)
			{			
				PIO_SetPeripheral(PIN_PA23, PIO_PERIPHERAL_C);
				PIO_EnablePeripheral(PIN_PA23);
			}
			if (Mask & 0x04)
			{			
				PIO_SetPeripheral(PIN_PA24, PIO_PERIPHERAL_C);
				PIO_EnablePeripheral(PIN_PA24);
			}
		}
		break;

		/* SERCOM4 -> PB10 (Tx), PB11 (Rx), PB14 (Alternative Tx) */
		case 4:
		{
			if (Mask & 0x01)
			{
				PIO_SetPeripheral(PIN_PB10, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PB10);
			}
			if (Mask & 0x02)
			{
				PIO_SetPeripheral(PIN_PB11, PIO_PERIPHERAL_D);
				PIO_EnablePeripheral(PIN_PB11);
			}
			if (Mask & 0x04)
			{
				PIO_SetPeripheral(PIN_PB14, PIO_PERIPHERAL_C);
				PIO_EnablePeripheral(PIN_PB14);
			}
		}
		break;

	}
}

 