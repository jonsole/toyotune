/*
 * evsys.c
 *
 * Created: 13/02/2020 20:16:52
 *  Author: WinUser
 */ 

#include "evsys.h"
#include "clk.h"

void EVSYS_Init(void)
{
 	/* Enable Event System Bus clock */
 	MCLK->APBCMASK.reg |= MCLK_APBCMASK_EVSYS;
}


void EVSYS_AsyncChannel(uint8_t Channel, uint8_t Generator, uint8_t User)
{
	CLK_EnablePeripheral(0, EVSYS_GCLK_ID_0 + Channel);
 	EVSYS->USER[User].reg = EVSYS_USER_CHANNEL(Channel + 1);
 	EVSYS->CHANNEL[Channel].reg = EVSYS_CHANNEL_EDGSEL_NO_EVT_OUTPUT | EVSYS_CHANNEL_PATH_ASYNCHRONOUS | EVSYS_CHANNEL_EVGEN(Generator);
}


void EVSYS_SyncChannel(uint8_t Channel, uint8_t Edge, uint8_t Generator, uint8_t User)
{
	CLK_EnablePeripheral(0, EVSYS_GCLK_ID_0 + Channel);
	EVSYS->USER[User].reg = EVSYS_USER_CHANNEL(Channel + 1);
	EVSYS->CHANNEL[Channel].reg = Edge | EVSYS_CHANNEL_PATH_SYNCHRONOUS | EVSYS_CHANNEL_EVGEN(Generator);
}