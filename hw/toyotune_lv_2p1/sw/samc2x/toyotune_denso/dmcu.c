/*
 * dmcu.c
 *
 * Created: 06/12/2020 14:28:06
 *  Author: WinUser
 */ 

#include "pio.h"
#include "dmcu.h"

 void DMCU_Init(void)
 {
 	/* !X_INIT - low output */
 	PIO_Clear(PIN_PA07);
 	PIO_EnableOutput(PIN_PA07);
}


void DMCU_ResetEnable(void)
{
 	PIO_Clear(PIN_PA07);
}


void DMCU_ResetDisable(void)
{
	PIO_Set(PIN_PA07);
}


bool DMCU_IsResetEnabled(void)
{
	return PIO_IsLow(PIN_PA07);
}


bool DMCU_IsHalted(void)
{
	return PIO_IsLow(PIN_PA20);
}


/* TODO: Locate in RAM */

void DMCU_ImageErase(void)
{
#if 0
	OS_InterruptDisable();
	
 uint32_t addr = FLASH_ADDR + app_block_index * ERASE_BLOCK_SIZE;
 uint32_t *flash_offset = (uint32_t *)addr;
 uint32_t *flash_data = (uint32_t *)app_flash_buf;

 if (-1 == app_block_index)
 return;

 NVMCTRL->ADDR.reg = addr >> 1;

 NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_UR;
 while (0 == NVMCTRL->INTFLAG.bit.READY);

 NVMCTRL->CTRLA.reg = NVMCTRL_CTRLA_CMDEX_KEY | NVMCTRL_CTRLA_CMD_ER;
 while (0 == NVMCTRL->INTFLAG.bit.READY);


 for (int page = 0; page < PAGES_IN_ERASE_BLOCK; page++)
 {
	 for (int i = 0; i < FLASH_PAGE_SIZE_WORDS; i++)
	 *flash_offset++ = *flash_data++;

	 while (0 == NVMCTRL->INTFLAG.bit.READY);
	 	
	
	
	OS_InterruptEnable();
#endif		
}


void DMCU_ImageWrite(const uint8_t *Buffer, uint8_t *BufferSize)
{
}