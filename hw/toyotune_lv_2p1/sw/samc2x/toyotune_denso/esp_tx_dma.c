#include "dmac.h"
#include "esp.h"

void ESP_HwTxDmaSync(ESP_t *Esp)
{
	DMAC_Descriptor_t *Desc = DMAC_ChannelGetBaseDescriptor(Esp->Hw.TxUsartDmaChannel);
	uint16_t ByteCount = DMAC_ChannelGetByteCount(Esp->Hw.TxUsartDmaChannel);
	uint8_t *TxAddress = (uint8_t *)Desc->SRCADDR.reg - ByteCount;
	BufferSetOutdexFromAddress(Esp->Hw.TxUsartBuffer, TxAddress);
}

void ESP_HwTxKick(ESP_t *Esp)
{
	/* Disable DMA interrupts on Tx channel */
	DMAC->CHID.reg = Esp->Hw.TxUsartDmaChannel;
	DMAC->CHINTENCLR.reg = DMAC_CHINTENCLR_MASK;

	/* Check the DMA transfer is not in progress or pending */
	const uint32_t Busy = (DMAC->BUSYCH.reg | DMAC->PENDCH.reg) & (1 << Esp->Hw.TxUsartDmaChannel);
	if (!Busy)
	{
		/* Check if there is actually anything to transfer in buffer */
		const uint16_t Amount = BufferAmount(Esp->Hw.TxUsartBuffer);
		if (Amount)
		{
			const uint16_t AmountToWrap = BufferAmountToWrap(Esp->Hw.TxUsartBuffer);

			/* Select channel and reset it */
			DMAC->CHID.reg = Esp->Hw.TxUsartDmaChannel;
			DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
			DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

			/* Configure descriptor */
			DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(Esp->Hw.TxUsartDmaChannel);
			DmaDesc->BTCTRL.reg = DMAC_BTCTRL_BEATSIZE_BYTE | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_VALID;
			DmaDesc->BTCNT.reg = (Amount > AmountToWrap) ? AmountToWrap : Amount;
			DmaDesc->SRCADDR.reg = (uint32_t)(Esp->Hw.TxUsartBuffer.Buffer + BufferOutdex(Esp->Hw.TxUsartBuffer) + DmaDesc->BTCNT.reg);
			DmaDesc->DSTADDR.reg = (uint32_t)&Esp->Hw.Usart->DATA;
			DmaDesc->DESCADDR.reg = 0;
		
			/* Configure channel, enable DMA interrupts and start transfer */			
			DMAC->CHCTRLB.reg = DMAC_CHCTRLB_TRIGACT_BEAT | DMAC_CHCTRLB_TRIGSRC(Esp->Hw.TxUsartDmaTrigger);
			DMAC->CHINTENSET.reg = DMAC_CHINTENSET_MASK;
			DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;
		}
	}
	else
	{	
		/* Enable DMA interrupts */
		DMAC->CHINTENSET.reg = DMAC_CHINTENSET_MASK;
	}	
}


