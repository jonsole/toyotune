/*
 * sdl.c
 *
 * Created: 19/10/2016 20:20:27
 *  Author: Jon
 */

#include "sdl.h"
#include "sercom.h"
#include "dmac.h"
#include "mem.h"
#include "debug.h"
#include "sercom.h"
#include "os.h"
#include "clk.h"
#include "evsys.h"

SDL_t *Sdl_Tc[2];

static void SDL_DmaIdle(SDL_t *Sdl)
{
	OS_InterruptDisable();

	/* Save current channel */
	uint8_t ChannelId = DMAC->CHID.reg;

	/* Select our DMA channel */
	DMAC->CHID.reg = Sdl->RxUsartDmaChannel;
	DMAC->CHCTRLA.reg = 0;

	DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(Sdl->RxUsartDmaChannel);
	
	/* Get transferred size */
	const uint16_t TotalSize =  DmaDesc->BTCNT.reg;
	const uint16_t WriteSize =  DMAC_ChannelGetWriteBackDescriptor(Sdl->RxUsartDmaChannel)->BTCNT.reg;
	const uint16_t RxSize = TotalSize - WriteSize;

	/* Restart DMA to other buffer */
	Sdl->DmaBufferIndex ^= 1;
	DmaDesc->DSTADDR.reg = (uint32_t)Sdl->DmaBuffer[Sdl->DmaBufferIndex] + DmaDesc->BTCNT.reg;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;

	/* Restore current channel */
	DMAC->CHID.reg = ChannelId;

	OS_InterruptEnable();

	/* Call callback function if set */
	if (Sdl->Callback)
		Sdl->Callback(Sdl, Sdl->CallbackData, Sdl->DmaBuffer[Sdl->DmaBufferIndex ^ 1], RxSize);	
}


void TC0_Handler(void) __attribute__ ((__interrupt__));
void TC0_Handler(void)
{
	uint32_t Status = TC0->COUNT16.INTFLAG.reg;
	if (Status & TC_INTFLAG_OVF)
		SDL_DmaIdle(Sdl_Tc[0]);
	
	TC0->COUNT16.INTFLAG.reg = Status;
}


void TC1_Handler(void) __attribute__ ((__interrupt__));
void TC1_Handler(void)
{
	uint32_t Status = TC1->COUNT16.INTFLAG.reg;
	if (Status & TC_INTFLAG_OVF)
		SDL_DmaIdle(Sdl_Tc[1]);
	
	TC1->COUNT16.INTFLAG.reg = Status;
}


void SDL_Init(SDL_t *Sdl, uint8_t DmaChannel, uint8_t Instance, uint8_t RxPad, uint8_t DmaSize, void (*Callback)(SDL_t *, void *, const uint8_t *, uint8_t), void *CallbackData)
{
 	Sdl->Usart = &SERCOM_GetSercom(Instance)->USART;
 	Sdl->Instance = Instance;
	Sdl->DmaSize = DmaSize;
	Sdl->DmaBuffer[0] = (uint8_t *)MEM_Alloc(DmaSize);
	Sdl->DmaBuffer[1] = (uint8_t *)MEM_Alloc(DmaSize);
	Sdl->DmaBufferIndex = 0;
	PanicNull(Sdl->DmaBuffer);
	Sdl->Callback = Callback;
	Sdl->CallbackData = CallbackData;

 	/* Allocate DMA channel 0 for USART receive */
 	Sdl->RxUsartDmaChannel = DMAC_ChannelAllocate(NULL, Sdl, DmaChannel);
	PanicFalse(Sdl->RxUsartDmaChannel == DmaChannel);
 	Sdl->RxUsartDmaTrigger = SERCOM_DmaRxTrigger(Sdl->Instance);

 	/* Enable USART clock in PMC */
 	SERCOM_EnableClock(Sdl->Instance);

 	/* Configure XCK & RX PIOs for this USART */
 	SERCOM_ConfigurePios(Sdl->Instance, (1 << 1) + (1 << RxPad));

 	/* Initialise USART. Synchronous, external clock, sample on rising edge */
 	SERCOM_UsartInit(Sdl->Instance,
 					 SERCOM_USART_CTRLA_DORD | SERCOM_USART_CTRLA_CPOL | SERCOM_USART_CTRLA_CMODE | SERCOM_USART_CTRLA_FORM(0) | SERCOM_USART_CTRLA_RXPO(RxPad) | SERCOM_USART_CTRLA_TXPO(0) | SERCOM_USART_CTRLA_MODE(0),
 					 SERCOM_USART_CTRLB_RXEN | SERCOM_USART_CTRLB_CHSIZE(1),
 					 0,
 					 0);
					  
	/* Enable USART */
	SERCOM_UsartEnable(Sdl->Usart);
					  
	/* Select channel and reset it */
	DMAC->CHID.reg = Sdl->RxUsartDmaChannel;
	DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

	/* Initialise receive descriptor */
	DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(Sdl->RxUsartDmaChannel);
	DmaDesc->BTCTRL.reg = DMAC_BTCTRL_BEATSIZE_BYTE | DMAC_BTCTRL_DSTINC | DMAC_BTCTRL_BLOCKACT_NOACT | DMAC_BTCTRL_VALID | DMAC_BTCTRL_EVOSEL_BEAT;
	DmaDesc->BTCNT.reg = Sdl->DmaSize;
	DmaDesc->SRCADDR.reg = (uint32_t)&Sdl->Usart->DATA;
	DmaDesc->DSTADDR.reg = (uint32_t)Sdl->DmaBuffer[Sdl->DmaBufferIndex] + DmaDesc->BTCNT.reg;
	DmaDesc->DESCADDR.reg = (uint32_t)DmaDesc;

	/* Configure and start transfer */
	DMAC->CHCTRLB.reg = DMAC_CHCTRLB_TRIGACT_BEAT | DMAC_CHCTRLB_LVL(3) | DMAC_CHCTRLB_TRIGSRC(Sdl->RxUsartDmaTrigger) | DMAC_CHCTRLB_EVOE;
	DMAC->CHINTENSET.reg = DMAC_CHINTENSET_MASK;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;

	Sdl_Tc[DmaChannel] = Sdl;
	if (DmaChannel == 0)
	{		
		/* Enable TC0 Bus clock */
		MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC0;

		/* Enable 1MHz GCLK1 for TC0 */
		CLK_EnablePeripheral(1, TC0_GCLK_ID);

		/* Configure TC0 to count down and generate interrupt when timer underflows.
		   Timer is re-triggered USART Rx DMAC beat event so that interrupt is only generated when USART
		   has been idle for 100uS */ 
		TC0->COUNT8.CTRLA.reg = TC_CTRLA_MODE_COUNT8 | TC_CTRLA_PRESCALER_DIV1; 
		TC0->COUNT8.CTRLBSET.reg = TC_CTRLBSET_ONESHOT | TC_CTRLBCLR_DIR;
		TC0->COUNT8.WAVE.reg = TC_WAVE_WAVEGEN_NFRQ;
		TC0->COUNT8.PER.reg = TC0->COUNT8.COUNT.reg = 100;
		TC0->COUNT8.EVCTRL.reg = TC_EVCTRL_TCEI | TC_EVCTRL_EVACT_RETRIGGER;

		/* Enable update interrupt */
		TC0->COUNT8.INTENSET.reg = TC_INTENSET_OVF;
		NVIC_SetPriority(TC0_IRQn, 1);
		NVIC_EnableIRQ(TC0_IRQn);

		/* Enable TC0 */
		TC0->COUNT8.CTRLA.reg |= TC_CTRLA_ENABLE;

		/* Re-trigger timer on DMAC channel 0 event */
		EVSYS_AsyncChannel(1, EVSYS_ID_GEN_DMAC_CH_0, EVSYS_ID_USER_TC0_EVU);
	}
	else if (DmaChannel == 1)
	{
		/* Enable TC1 Bus clock */
		MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC1;

		/* Enable 1MHz GCLK1 for TC1 */
		CLK_EnablePeripheral(1, TC1_GCLK_ID);

		/* Configure TC1 to count down and generate interrupt when timer underflows.
		   Timer is re-triggered USART Rx DMAC beat event so that interrupt is only generated when USART
		   has been idle for 100uS */ 
		TC1->COUNT8.CTRLA.reg = TC_CTRLA_MODE_COUNT8 | TC_CTRLA_PRESCALER_DIV1; 
		TC1->COUNT8.CTRLBSET.reg = TC_CTRLBSET_ONESHOT | TC_CTRLBCLR_DIR;
		TC1->COUNT8.WAVE.reg = TC_WAVE_WAVEGEN_NFRQ;
		TC1->COUNT8.PER.reg = TC1->COUNT8.COUNT.reg = 100;
		TC1->COUNT8.EVCTRL.reg = TC_EVCTRL_TCEI | TC_EVCTRL_EVACT_RETRIGGER;

		/* Enable update interrupt */
		TC1->COUNT8.INTENSET.reg = TC_INTENSET_OVF;
		NVIC_SetPriority(TC1_IRQn, 1);
		NVIC_EnableIRQ(TC1_IRQn);

		/* Enable TC1 */
		TC1->COUNT8.CTRLA.reg |= TC_CTRLA_ENABLE;

		/* Re-trigger timer on DMAC channel 1 event */
		EVSYS_AsyncChannel(2, EVSYS_ID_GEN_DMAC_CH_1, EVSYS_ID_USER_TC1_EVU);
	}
	else
		Panic();
}
