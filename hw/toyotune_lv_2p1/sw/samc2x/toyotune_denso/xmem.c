/*
 * xmem.c
 *
 * Created: 08/11/2020 17:07:38
 *  Author: jonso
 */ 
#include "xmem.h"
#include "pio.h"
#include "dmcu.h"

#define XMEM_PIN_RD		(PIN_PB16)
#define XMEM_PIN_WR		(PIN_PB17)
#define XMEM_PIN_ADR	(PIN_PB30)

void XMEM_Init(void)
{
	/* Initialise PIOs */

	/* !X_RD - high output */
	PIO_Set(XMEM_PIN_RD);
	PIO_EnableOutput(XMEM_PIN_RD);
	
	/* !X_WR - high output */
	PIO_Set(XMEM_PIN_WR);
	PIO_EnableOutput(XMEM_PIN_WR);
	
	/* X_ADR - low output */
	PIO_Clear(XMEM_PIN_ADR);
	PIO_EnableOutput(XMEM_PIN_ADR);	
	
	/* Enable PIN inputs for DA0:7 */
	for (int Pin = PIN_PB00; Pin <= PIN_PB07; Pin++)
		PIO_EnableInput(Pin);
}


void XMEM_BlockWrite(uint16_t DestAddr, const uint8_t *SrcPtr, uint16_t Size)
{	
	/* TODO: Check Denso MCU is held in reset */
	//assert(DMCU_IsResetEnabled());
	
	PortGroup *Group = &PORT->Group[PIO_GetPioGroup(PIN_PB00)];
	
	while (Size--)
	{
		/* Set DA0:7 and A8:A15 as outputs */
		Group->DIRSET.reg = 0xFFFF;

		/* Drive address */
		Group->OUT.reg = (Group->OUT.reg & 0xFFFF0000UL) | DestAddr;
	
		/* Pulse ADR high to latch A0:A7 */
		PIO_Set(XMEM_PIN_ADR);
		PIO_Clear(XMEM_PIN_ADR);
	
		/* Set DA0:7 to value to write */
		Group->OUT.reg = (Group->OUT.reg & 0xFFFFFF00UL) | *SrcPtr;
	
		/* Pulse WR low to perform write*/
		PIO_Clear(XMEM_PIN_WR);
		PIO_Set(XMEM_PIN_WR);
	
		SrcPtr += 1;
		DestAddr += 1;
	}

	/* Set DA0:7 and A8:A15 as inputs */
	Group->DIRCLR.reg = 0xFFFF;
	Group->OUTCLR.reg = 0xFFFF;
}



void XMEM_BlockRead(uint8_t *DestPtr, uint16_t SrcAddr, uint16_t Size)
{	
	/* TODO: Check Denso MCU is held in reset */
	//assert(DMCU_IsResetEnabled());
	
	volatile PortGroup *Group = &PORT->Group[PIO_GetPioGroup(PIN_PB00)];
	
	while (Size--)
	{
		/* Set DA0:7 and A8:A15 as outputs */
		Group->DIRSET.reg = 0xFFFF;

		/* Drive address */
		Group->OUT.reg = (Group->OUT.reg & 0xFFFF0000UL) | SrcAddr;
	
		/* Pulse ADR high to latch A0:A7 */
		PIO_Set(XMEM_PIN_ADR);
		PIO_Clear(XMEM_PIN_ADR);
	
		/* Set DA0:7 as inputs */
		Group->DIRCLR.reg = 0xFF;
		
		/* Pulse RD low to perform read */
		PIO_Clear(XMEM_PIN_RD);
		*DestPtr = Group->IN.reg & 0xFF;
		PIO_Set(XMEM_PIN_RD);
	
		SrcAddr += 1;
		DestPtr += 1;
	}

	/* Set DA0:7 and A8:A15 as inputs */
	Group->DIRCLR.reg = 0xFFFF;
	Group->OUTCLR.reg = 0xFFFF;
}


void XMEM_WriteEnable(bool Enabled)
{
	uint8_t MemC = Enabled ? 0x01 : 0x00;
	XMEM_BlockWrite(DMCU_REG_MEMC, &MemC, sizeof(MemC));
}


void XMEM_AddressTest(void)
{
	uint16_t Address = 0;
	PortGroup *Group = &PORT->Group[PIO_GetPioGroup(PIN_PB00)];
	
	/* Set DA0:7 and A8:A15 as outputs */
	Group->DIRSET.reg = 0xFFFF;
	while (1)
	{
		/* Drive address */
		Group->OUT.reg = (Group->OUT.reg & 0xFFFF0000UL) | Address;
		Address += 1;
	}
}


void XMEM_PinTest(uint8_t Pin)
{
	while (1)
	{
		PIO_Set(Pin);
		PIO_Clear(Pin);
	}
}


