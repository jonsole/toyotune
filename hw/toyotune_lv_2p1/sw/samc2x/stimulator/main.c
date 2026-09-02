/*
 * toyotune_denso.c
 *
 * Created: 27/05/2016 20:20:53
 * Author : Jon
 */ 

#include <sam.h>

#include "debug.h"
#include "dmac.h"
#include "clk.h"
#include "pio.h"
#include "vrg.h"
#include "igt.h"
#include "can.h"
#include "knock.h"
#include "spi_dac.h"
#include "map.h"
#include "throttle.h"

void TC0_Handler(void)  __attribute__((__interrupt__));
void TC0_Handler(void)
{
	uint16_t cc0 = TC0->COUNT16.CC[0].reg;
	uint16_t cc1 = TC0->COUNT16.CC[1].reg;
	(void)cc0;
	(void)cc1;
}

void EIC_Handler(void) __attribute__ ((__interrupt__));
void EIC_Handler(void)
{
	uint32_t Status = EIC->INTFLAG.reg;
	EIC->INTFLAG.reg = Status;
}

void EVSYS_Handler(void) __attribute__ ((__interrupt__));
void EVSYS_Handler(void)
{
	while (1);
}

volatile uint32_t UsPerRev;
volatile int32_t Timing;

volatile uint8_t Digit_Data[4];






// PA18 - VRG Output
// PA19 - VRG Output
// PA20 - VRG Output - 10 BTDC - 12.5 ATDC
// PA22 - Debug TX
// PA23 - Debug Rx
// PA24	- CAN
// PA25 - CAN
// PA27 - PIM DAC CS

// PB00 - SPI_DAC PAD 2 - MOSI
// PB01 - SPI_DAC PAD 3 - CLK
// PB03 - SPI_DAC 
// PB05 - VTA DAC CS
// PB08 - IGT Input
// PB09 - IGT Sync - 10 BTDC - 12.5 ATDC Input
// PB16 - VRG Output
// PB17 - VRG Output
// PB30 - VRG Output
// PB31 - VRG Output


int main(void)
{
	PAC->WRCTRL.reg = PAC_WRCTRL_PERID(ID_DSU) | PAC_WRCTRL_KEY_CLR;
	MCLK->APBCMASK.reg |= MCLK_AHBMASK_NVMCTRL | MCLK_AHBMASK_DSU;
	MCLK->APBBMASK.reg |= MCLK_APBBMASK_NVMCTRL | MCLK_APBBMASK_DSU;
  	
	CLK_Init();

    /* Initialize the SAM system */
    SystemInit();

	CAN_Init();

	/* Initialise DMA controller */
	DMAC_Init();

	SPIDAC_Init();
	
	VRG_Init();

	Knock_Init();
	
	IGT_Init();

	/* Initialise debug output */
	//Debug_Init();
	//Debug("Stimulator SAMC21 v0.1\n");


	//SysTick_Config(48000000 / 1000);
	//NVIC_EnableIRQ(SysTick_IRQn);

	Throttle_Init();

	MAP_Init();
		
	VRG_SetRpm(1000);

    while (1)
	{
		static uint32_t CAN_TestTxId = 0;

		CAN_Tx(CAN_TestTxId++, "Hello", 5);
	

		if (IGT_GetTimingPeriod() != 0)
		{
			UsPerRev = 60000000UL / VRG_GetRpm();
			Timing = (36000UL * IGT_GetTimingPeriod() / UsPerRev) - 1250;

			Debug("%d uS, %d\n", IGT_GetTimingPeriod(), Timing);
			IGT_ClearTimingPeriod();
		}
	}
}
