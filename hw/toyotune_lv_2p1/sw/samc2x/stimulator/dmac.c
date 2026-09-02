/*
 * dmac.c
 *
 * Created: 27/05/2016 21:55:12
 *  Author: Jon
 */ 

#include <string.h>
#include "dmac.h"

static uint16_t DMAC_AllocateChannels;

DMAC_Descriptor_t DMAC_DescriptorArray[12] __attribute((aligned (8)));
DMAC_Descriptor_t DMAC_WriteBackDescriptorArray[12] __attribute((aligned (8)));

void (*DMAC_InterruptHandler[12])(void *, const uint8_t, const uint16_t);
void *DMAC_InterruptData[12];

void DMAC_Handler(void)  __attribute__((__interrupt__));
void DMAC_Handler(void)
{
	/* Save current channel */
	uint8_t ChannelId = DMAC->CHID.reg;

	/* Get pending interrupt for lowest channel */
	uint16_t IntPending = DMAC->INTPEND.reg;
	
	/* Loop whilst there are interrupts still pending */
	while (IntPending & (DMAC_INTPEND_SUSP | DMAC_INTPEND_TCMPL | DMAC_INTPEND_TERR))
	{
		const uint8_t IntChannel = (IntPending & DMAC_INTPEND_ID_Msk) >> DMAC_INTPEND_ID_Pos;
		DMAC->CHID.reg = IntChannel;
		DMAC_InterruptHandler[IntChannel](DMAC_InterruptData[IntChannel], IntChannel, IntPending);
	
		/* Get next pending interrupt */
		IntPending = DMAC->INTPEND.reg;
	}

	/* Restore channel ID */
	DMAC->CHID.reg = ChannelId;
}


void DMAC_Init(void)
{
	/* Ensure descriptors are clear */
    memset(DMAC_DescriptorArray, 0, sizeof(DMAC_DescriptorArray));
    memset(DMAC_WriteBackDescriptorArray, 0, sizeof(DMAC_WriteBackDescriptorArray));
	
	/* Enable DMA clocks */
	MCLK->AHBMASK.reg |= MCLK_AHBMASK_DMAC;
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_DAC;

	/* Make sure DMAC is disabled */
	DMAC->CTRL.bit.DMAENABLE = 0;
	DMAC->CTRL.bit.SWRST = 1;

	/* Set BASEADDR & WRBADDR */
	DMAC->BASEADDR.reg = (uint32_t)DMAC_DescriptorArray;
	DMAC->WRBADDR.reg  = (uint32_t)DMAC_WriteBackDescriptorArray;

	/* DMA continues when CPU is halted by external debugger */
	//DMAC->DBGCTRL.reg = DMAC_DBGCTRL_DBGRUN;

	/* Enable DMAC and all priority levels */
	DMAC->CTRL.reg = DMAC_CTRL_DMAENABLE | DMAC_CTRL_LVLEN(0xF);

	/* Enable DMAC interrupts, priority 2 */
	NVIC_SetPriority(DMAC_IRQn, 2);
	NVIC_EnableIRQ(DMAC_IRQn);
}


uint8_t DMAC_ChannelAllocate(void (*InterruptHandler)(void *, const uint8_t, const uint16_t), void *InterruptData)
{
	for (uint8_t Channel = 4; Channel < 12; Channel++)
	{
		const uint16_t ChannelMask = 1U << Channel;
		if (~DMAC_AllocateChannels & ChannelMask)
		{
			DMAC_AllocateChannels |= ChannelMask;
			DMAC_InterruptHandler[Channel] = InterruptHandler;
			DMAC_InterruptData[Channel] = InterruptData;
			return Channel;
		}
	}

	return 0xFF;
}


void DMAC_ChannelFree(uint8_t Channel)
{
	const uint16_t ChannelMask = 1U << Channel;
	DMAC_AllocateChannels &= ~ChannelMask;
}


DMAC_Descriptor_t *DMAC_ChannelGetBaseDescriptor(uint8_t Channel)
{
	DMAC_Descriptor_t *DescriptorArray = (DMAC_Descriptor_t *)DMAC->BASEADDR.reg;
	return &DescriptorArray[Channel];
}

DMAC_Descriptor_t *DMAC_ChannelGetWriteBackDescriptor(uint8_t Channel)
{
	DMAC_Descriptor_t *DescriptorArray = (DMAC_Descriptor_t *)DMAC->WRBADDR.reg;
	return &DescriptorArray[Channel];
}

uint16_t DMAC_ChannelGetByteCount(uint8_t Channel)
{
	DMAC_ACTIVE_Type Active = DMAC->ACTIVE;
	
	if (Active.bit.ABUSY && Active.bit.ID == Channel)
		return Active.bit.BTCNT;
	else
		return DMAC_ChannelGetWriteBackDescriptor(Channel)->BTCNT.reg;
}

