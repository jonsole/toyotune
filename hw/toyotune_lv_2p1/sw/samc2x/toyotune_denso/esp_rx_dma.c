#include "esp.h"
#include "dmac.h"
#include "pio.h"
#include "evsys.h"
#include "os.h"
#include "task_id.h"


void ESP_HwRxDmaSync(ESP_t *Esp)
{
	DMAC_Descriptor_t *Desc = DMAC_ChannelGetBaseDescriptor(Esp->Hw.RxUsartDmaChannel);
	uint16_t ByteCount = DMAC_ChannelGetByteCount(Esp->Hw.RxUsartDmaChannel);
	uint8_t *RxAddress = (uint8_t *)Desc->DSTADDR.reg - ByteCount;
	BufferSetIndexFromAddress(Esp->Hw.RxUsartBuffer, RxAddress);
}


void TC0_Handler(void) __attribute__ ((__interrupt__));
void TC0_Handler(void)
{
	uint32_t Status = TC0->COUNT16.INTFLAG.reg;
	if (Status & TC_INTFLAG_OVF)
		OS_SignalSend(ESP_TASK_ID, ESP_SIGNAL_RX_IDLE);
	TC0->COUNT16.INTFLAG.reg = Status;
}

void ESP_HwRxDmaStart(ESP_t *Esp)
{
	/* Select channel and reset it */
	DMAC->CHID.reg = Esp->Hw.RxUsartDmaChannel;
	DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

	/* Initialise receive descriptor */
	DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(Esp->Hw.RxUsartDmaChannel);
	DmaDesc->BTCTRL.reg = DMAC_BTCTRL_BEATSIZE_BYTE | DMAC_BTCTRL_DSTINC | DMAC_BTCTRL_BLOCKACT_NOACT | DMAC_BTCTRL_VALID | DMAC_BTCTRL_EVOSEL_BEAT; 
	DmaDesc->BTCNT.reg = BufferSize(Esp->Hw.RxUsartBuffer);
	DmaDesc->SRCADDR.reg = (uint32_t)&Esp->Hw.Usart->DATA;
	DmaDesc->DSTADDR.reg = (uint32_t)Esp->Hw.RxUsartBuffer.Buffer + DmaDesc->BTCNT.reg;
	DmaDesc->DESCADDR.reg = (uint32_t)DmaDesc;

	/* Configure and start transfer */
	DMAC->CHCTRLB.reg = DMAC_CHCTRLB_TRIGACT_BEAT | DMAC_CHCTRLB_LVL(3) | DMAC_CHCTRLB_TRIGSRC(Esp->Hw.RxUsartDmaTrigger) | DMAC_CHCTRLB_EVOE;
	DMAC->CHINTENCLR.reg = DMAC_CHINTENCLR_MASK;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;
	
	/* Toggle PA15/LED on interrupt */
	PIO_EnableOutput(PIN_PA15);

	/* Enable TCC0 Bus clock */
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_TC0;

	/* Enable 1MHz GCLK1 for TC0 */
	CLK_EnablePeripheral(1, TC0_GCLK_ID);

	/* Configure TC0 to count down and generate interrupt when timer underflows.
	   Timer is re-triggered USART Rx DMAC beat event so that interrupt is only generated when USART
	   has been idle for 200uS */ 
	TC0->COUNT8.CTRLA.reg = TC_CTRLA_MODE_COUNT8 | TC_CTRLA_PRESCALER_DIV1; 
	TC0->COUNT8.CTRLBSET.reg = TC_CTRLBSET_ONESHOT | TC_CTRLBCLR_DIR;
	TC0->COUNT8.WAVE.reg = TC_WAVE_WAVEGEN_NFRQ;
	TC0->COUNT8.PER.reg = TC0->COUNT8.COUNT.reg = 200;
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

