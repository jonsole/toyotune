#include "debug.h"
#include "dmac.h"
#include "buffer.h"
#include "sercom.h"
#include "os.h"

#if 0

Debug_t DebugState;

static void Debug_TxDmaSync(void)
{
	DMAC_Descriptor_t *Desc = DMAC_ChannelGetBaseDescriptor(DebugState.TxUsartDmaChannel);
	uint16_t ByteCount = DMAC_ChannelGetByteCount(DebugState.TxUsartDmaChannel);
	uint8_t *TxAddress = (uint8_t *)Desc->SRCADDR.reg - ByteCount;
	BufferSetOutdexFromAddress(DebugState.Buffer, TxAddress);
}

static void Debug_TxKick(void)
{
	if (DebugState.Usart)
	{
		OS_InterruptDisable();

		/* Can only start DMA if transmit isn't in progress */
		if (!DebugState.TxInProgress)
		{
			/* Check if there is actually anything to transfer in buffer */
			const uint16_t Amount = BufferAmount(DebugState.Buffer);
			if (Amount)
			{
				const uint16_t AmountToWrap = BufferAmountToWrap(DebugState.Buffer);
			
				/* Update descriptor */
				DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(DebugState.TxUsartDmaChannel);
				DmaDesc->BTCNT.reg = (Amount > AmountToWrap) ? AmountToWrap : Amount;
				DmaDesc->SRCADDR.reg = (uint32_t)(DebugState.Buffer.Buffer + BufferOutdex(DebugState.Buffer) + DmaDesc->BTCNT.reg);
	
				/* Start transfer */
				uint8_t ChannelId = DMAC->CHID.reg;
				DMAC->CHID.reg = DebugState.TxUsartDmaChannel;
				DMAC->CHCTRLA.reg = DMAC_CHCTRLA_ENABLE;
				DMAC->CHID.reg = ChannelId;
				
				/* Set in progress flag */
				DebugState.TxInProgress = 1;
			}
		}
	
		OS_InterruptEnable();
	}
}

static void Debug_InterruptHandler(void *DebugVoid, uint8_t DmaChannel, uint16_t IntPending)
{
	const uint8_t IntStatus = DMAC->CHINTFLAG.reg;
	if (IntStatus & DMAC_CHINTFLAG_TCMPL)
	{
		/* Synchronise buffer outdex with PDC transmit address */
		Debug_TxDmaSync();

		/* Clear in progress flag */
		DebugState.TxInProgress = 0;

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
	if ((Char == '\n') || (BufferAmount(DebugState.Buffer) > 80))
		Debug_TxKick();
}

void Debug_Init(uint8_t Instance)
{
	DebugState.Usart = &SERCOM_GetSercom(Instance)->USART;
	DebugState.Instance = Instance;

	/* Allocate DMA channels for USART transmit and receive */
	DebugState.RxUsartDmaChannel = DMAC_ChannelAllocate(NULL, NULL, DMAC_NO_CHANNEL);
	DebugState.TxUsartDmaChannel = DMAC_ChannelAllocate(Debug_InterruptHandler, NULL, DMAC_NO_CHANNEL);
	DebugState.TxUsartDmaTrigger = SERCOM_DmaTxTrigger(DebugState.Instance);
	DebugState.RxUsartDmaTrigger = SERCOM_DmaRxTrigger(DebugState.Instance);

	DMAC_Descriptor_t *DmaDesc = DMAC_ChannelGetBaseDescriptor(DebugState.TxUsartDmaChannel);
	DmaDesc->BTCTRL.reg = DMAC_BTCTRL_BEATSIZE_BYTE | DMAC_BTCTRL_SRCINC | DMAC_BTCTRL_BLOCKACT_INT | DMAC_BTCTRL_VALID;
	DmaDesc->DSTADDR.reg = (uint32_t)&DebugState.Usart->DATA;
	DmaDesc->DESCADDR.reg = 0;

	/* Reset channel */
	DMAC->CHID.reg = DebugState.TxUsartDmaChannel;
	DMAC->CHCTRLA.reg &= ~DMAC_CHCTRLA_ENABLE;
	DMAC->CHCTRLA.reg = DMAC_CHCTRLA_SWRST;

	/* Configure and enable DMA interrupt */
	DMAC->CHCTRLB.reg = DMAC_CHCTRLB_TRIGACT_BEAT | DMAC_CHCTRLB_TRIGSRC(DebugState.TxUsartDmaTrigger);
	DMAC->CHINTENSET.reg = DMAC_CHINTENSET_MASK;

	/* Enable USART clock in PMC */
	SERCOM_EnableClock(DebugState.Instance);

	/* Configure PIOs for this USART */
	SERCOM_ConfigurePios(DebugState.Instance, 7);

	/* Initialise USART */
	SERCOM_UsartInit(DebugState.Instance,
					 SERCOM_USART_CTRLA_DORD | SERCOM_USART_CTRLA_RXPO(1) | SERCOM_USART_CTRLA_TXPO(0) | SERCOM_USART_CTRLA_MODE(1),
					 /*SERCOM_USART_CTRLB_RXEN | */SERCOM_USART_CTRLB_TXEN | SERCOM_USART_CTRLB_CHSIZE(0),
					 0,
					 63019);  // 115200,8,n,1

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


void PanicDebug(const char *FileName, int Line)
{
	Debug("Panic, %s:%u", FileName, Line);
	for(;;);
}

#endif
