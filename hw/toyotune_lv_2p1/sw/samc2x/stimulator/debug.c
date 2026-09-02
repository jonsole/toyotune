#include "debug.h"
#include "dmac.h"
#include "buffer.h"
#include "sercom.h"

Debug_t DebugState;
volatile int DebugDmaPending = 0;

static void Debug_TxDmaSync(void)
{
	DMAC_Descriptor_t *Desc = DMAC_ChannelGetBaseDescriptor(DebugState.TxUsartDmaChannel);
	uint16_t ByteCount = DMAC_ChannelGetByteCount(DebugState.TxUsartDmaChannel);
	uint8_t *TxAddress = (uint8_t *)Desc->SRCADDR.reg - ByteCount;
	BufferSetOutdexFromAddress(DebugState.Buffer, TxAddress);
	DebugDmaPending--;
}

static void Debug_TxKick(void)
{
	if (DebugDmaPending)
		return;

	/* Save current channel */
	uint8_t ChannelId = DMAC->CHID.reg;

	/* Disable DMA interrupts on Tx channel */
	DMAC->CHID.reg = DebugState.TxUsartDmaChannel;
	DMAC->CHINTENCLR.reg = DMAC_CHINTENCLR_MASK;

	/* Check the DMA transfer is not in progress or pending */
	const uint32_t Busy = (DMAC->BUSYCH.reg | DMAC->PENDCH.reg) & (1 << DebugState.TxUsartDmaChannel);
	if (!Busy)
	{
		/* Check if there is actually anything to transfer in buffer */
		const uint16_t Amount = BufferAmount(DebugState.Buffer);
		if (Amount)
		{
			const uint16_t AmountToWrap = BufferAmountToWrap(DebugState.Buffer);

			/* Select channel and reset it */
			DMAC->CHID.reg = DebugState.TxUsartDmaChannel;
			DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
			DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

			/* Configure descriptor */
			DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(DebugState.TxUsartDmaChannel);
			DmaDesc->BTCTRL.reg = DMAC_BTCTRL_BEATSIZE_BYTE | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_VALID;
			DmaDesc->BTCNT.reg = (Amount > AmountToWrap) ? AmountToWrap : Amount;
			DmaDesc->SRCADDR.reg = (uint32_t)(DebugState.Buffer.Buffer + BufferOutdex(DebugState.Buffer) + DmaDesc->BTCNT.reg);
			DmaDesc->DSTADDR.reg = (uint32_t)&DebugState.Usart->DATA;
			DmaDesc->DESCADDR.reg = 0;

			/* Increment DMA pending counter */			
			DebugDmaPending++;
			if (DebugDmaPending > 1)
				Panic();

			/* Configure channel, enable DMA interrupts and start transfer */
			DMAC->CHCTRLB.reg = DMAC_CHCTRLB_TRIGACT_BEAT | DMAC_CHCTRLB_TRIGSRC(DebugState.TxUsartDmaTrigger);
			DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;
		}
	}

	/* (Re)enable DMA interrupts */
	DMAC->CHINTENSET.reg = DMAC_CHINTENSET_MASK;

	/* Restore channel ID */
	DMAC->CHID.reg = ChannelId;
}

static void Debug_InterruptHandler(void *DebugVoid, uint8_t DmaChannel, uint16_t IntPending)
{
	const uint8_t IntStatus = DMAC->CHINTFLAG.reg;
	if (IntStatus & DMAC_CHINTFLAG_TCMPL)
	{
		/* Synchronise buffer outdex with PDC transmit address */
		Debug_TxDmaSync();

		/* Re-start DMA if there's any more data in buffer */
		Debug_TxKick();
	}

	/* Clear all channel interrupts */
	DMAC->CHINTFLAG.reg = IntStatus;
}

void Debug_PutChar(char Char, void *Context)
{
	if (BufferSpace(DebugState.Buffer) < 2)
		return; 

	BufferWrite(DebugState.Buffer, Char);
	if (Char == '\n')
		Debug_TxKick();
	else if (BufferAmount(DebugState.Buffer) > 80)
		Debug_TxKick();
}

void Debug_Flush(void)
{
	Debug_TxKick();
}

uint8_t Debug_GetChar(void)
{
	if (!DebugState.Usart->INTFLAG.bit.RXC)
		return 0;

	const uint8_t Char = (uint8_t)DebugState.Usart->DATA.reg;

	/* Clear any framing/parity/overflow error left in STATUS, which is
	   write-one-to-clear. Left set, an overflow keeps the receiver wedged. */
	DebugState.Usart->STATUS.reg = DebugState.Usart->STATUS.reg;

	return Char;
}

void Debug_Init(void)
{
	DebugState.Instance = 3;
	DebugState.Usart = &(SERCOM_GetSercom(DebugState.Instance)->USART);

	/* Allocate DMA channels for USART transmit and receive */
	DebugState.RxUsartDmaChannel = DMAC_ChannelAllocate(NULL, NULL);
	DebugState.TxUsartDmaChannel = DMAC_ChannelAllocate(Debug_InterruptHandler, NULL);
	DebugState.TxUsartDmaTrigger = SERCOM_DmaTxTrigger(DebugState.Instance);
	DebugState.RxUsartDmaTrigger = SERCOM_DmaRxTrigger(DebugState.Instance);

	/* Enable USART clock in PMC */
	SERCOM_EnableClock(DebugState.Instance);

	/* Configure PIOs for this USART */
	SERCOM_ConfigurePios(DebugState.Instance);

	/* Initialise USART */
	SERCOM_UsartInit(DebugState.Instance,
					 SERCOM_USART_CTRLA_DORD | SERCOM_USART_CTRLA_RXPO(1) | SERCOM_USART_CTRLA_TXPO(0) | SERCOM_USART_CTRLA_MODE(1),
					 SERCOM_USART_CTRLB_RXEN | SERCOM_USART_CTRLB_TXEN | SERCOM_USART_CTRLB_CHSIZE(0),
					 0, 63019);  // 115200

	BufferInit(DebugState.Buffer);
	DebugState.Level = 0;

	/* Enable USART */
	SERCOM_UsartEnable(DebugState.Usart);
}

void Debug_SetLevel(uint8_t Level)
{
	DebugState.Level = Level;
}

uint8_t Debug_GetLevel(void)
{
	return DebugState.Level;
}
