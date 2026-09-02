/*
 * can.c
 *
 * Created: 08/06/2023 20:50:07
 *  Author: jonso
 */ 


#include <sam.h>
#include "config.h"
#include "pio.h"
#include <stdalign.h>
#include <string.h>

#include "can.h"

#define CAN_FILTER_ID         TOYOTUNE_CAN_ID_DIAG_CMD

// Nominal bit rate. The time quanta is 48 MHz / (5+1) = 8MHz.
// And each bit is (1 + NTSEG1 + 1 + NTSEG2 + 1) = 16 time quanta
// which means the bit rate is 8MHz / 16 = 500 KHz.
#define NBTP_NBRP_VALUE       5  // Nominal bit Baud Rate Prescaler
#define NBTP_NSJW_VALUE       3  // Nominal bit (Re)Synchronization Jump Width
#define NBTP_NTSEG1_VALUE     10 // Nominal bit Time segment before sample point
#define NBTP_NTSEG2_VALUE     3  // Nominal bit Time segment after sample point

// Data bit rate. The time quanta is 48 MHz / (0+1) =  48MHz.
// And each bit is (1 + DTSEG1 + 1 + DTSEG2 + 1) = 16 time quanta
// which means the bit rate is 48 MHz / 16 = 3 MHz.
#define DBTP_DBRP_VALUE       0  // Data bit Baud Rate Prescaler
#define DBTP_DSJW_VALUE       3  // Data bit (Re)Synchronization Jump Width
#define DBTP_DTSEG1_VALUE     10 // Data bit Time segment before sample point
#define DBTP_DTSEG2_VALUE     3  // Data bit Time segment after sample point


/* Message transmission buffer */
#define CAN_TX_BUFFER_SIZE	(0)
#define CAN_TX_QUEUE_SIZE	(8)
static alignas(4) CanMramTxbe CAN_TxBuffer[CAN_TX_BUFFER_SIZE + CAN_TX_QUEUE_SIZE];

/* Receive FIFO, for any processed messages */
#define CAN_RX_FIFO_FILTERED_SIZE (16)
static alignas(4) CanMramRxf0e CAN_RxFifo0Filtered[CAN_RX_FIFO_FILTERED_SIZE];

/* FIFO 1 is used as a dumping ground for all messages that are not processed
   but accepted by the Global Filter Configuration in order to receive interrupts
   at the end of every bus message for errata 1.6.18 Workaround 1 */
#define CAN_RX_FIFO_UNFILTERED_SIZE (2)
static alignas(4) CanMramRxf1e CAN_RxFifo1Unfiltered[CAN_RX_FIFO_UNFILTERED_SIZE];

/* One standard-identifier filter element, matching the diagnostic command
   frame and steering it into Rx FIFO 0.  Everything else continues to fall
   through GFC into FIFO 1, which is not a receive path but overwrite-mode
   scratch for the errata 1.6.18 workaround. */
static alignas(4) CanMramSidfe CAN_RxStandardFilter[1];

static CAN_RxHandler_t CAN_RxHandler;

void CAN_RxSetHandler(CAN_RxHandler_t Handler)
{
	CAN_RxHandler = Handler;
}



void CAN_Init(void)
{
	PIO_SetPeripheral(PIN_PA24G_CAN0_TX, MUX_PA24G_CAN0_TX);  
	PIO_EnablePeripheral(PIN_PA24G_CAN0_TX);
	PIO_SetPeripheral(PIN_PA25G_CAN0_RX, MUX_PA25G_CAN0_RX);
	PIO_EnablePeripheral(PIN_PA25G_CAN0_RX);

	MCLK->AHBMASK.reg |= MCLK_AHBMASK_CAN0;
	GCLK->PCHCTRL[CAN0_GCLK_ID].reg = GCLK_PCHCTRL_GEN(0) | GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[CAN0_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));

	/* Reset the CC Control Register */
	CAN0->CCCR.reg = CAN_CCCR_INIT;
	while (~CAN0->CCCR.reg & CAN_CCCR_INIT);
	
	/* Enable writing to configuration registers */
	CAN0->CCCR.reg = CAN_CCCR_INIT | CAN_CCCR_CCE;

	/* Configure the bit rate and sampling point */
	CAN0->NBTP.reg = CAN_NBTP_NBRP(NBTP_NBRP_VALUE) | CAN_NBTP_NSJW(NBTP_NSJW_VALUE) |
					 CAN_NBTP_NTSEG1(NBTP_NTSEG1_VALUE) | CAN_NBTP_NTSEG2(NBTP_NTSEG2_VALUE);
	CAN0->DBTP.reg = CAN_DBTP_DBRP(DBTP_DBRP_VALUE) | CAN_DBTP_DSJW(DBTP_DSJW_VALUE) |
					 CAN_DBTP_DTSEG1(DBTP_DTSEG1_VALUE) | CAN_DBTP_DTSEG2(DBTP_DTSEG2_VALUE);
	
	/* Global Filter Configuration:  */
	/* FIFO 1 is used as a dumping ground for all messages that are not processed
	   but accepted by the Global Filter Configuration in order to receive interrupts
	   at the end of every bus message for errata 1.6.18 Workaround 1 */
	CAN0->GFC.reg = CAN_GFC_ANFE_RXF1 | CAN_GFC_ANFS_RXF1;

	/* Standard-identifier filter list: accept the diagnostic command frame
	   into FIFO 0.  SFT_CLASSIC with SFID2 as a mask of all ones matches
	   that identifier exactly. */
	CAN_RxStandardFilter[0].SIDFE_0.reg = CAN_SIDFE_0_SFT_CLASSIC |
								  CAN_SIDFE_0_SFEC_STF0M |
								  CAN_SIDFE_0_SFID1(CAN_FILTER_ID) |
								  CAN_SIDFE_0_SFID2(0x7FF);
	CAN0->SIDFC.reg = CAN_SIDFC_FLSSA((uint32_t)CAN_RxStandardFilter) |
					  CAN_SIDFC_LSS(1);

	/* Rx FIFO 0 - this could be used for receiving messages to be processed */
	CAN0->RXF0C.reg = CAN_RXF0C_F0SA((uint32_t)CAN_RxFifo0Filtered) | CAN_RXF0C_F0S(CAN_RX_FIFO_FILTERED_SIZE);

	/* Rx FIFO 1 Config */
	/* FIFO 1 is used as a dumping ground for all messages that are not processed
		but accepted by the Global Filter Configuration in order to receive interrupts
		at the end of every bus message for errata 1.6.18 Workaround 1 */
	CAN0->RXF1C.reg = CAN_RXF1C_F1SA((uint32_t)CAN_RxFifo1Unfiltered) | CAN_RXF1C_F1S(CAN_RX_FIFO_UNFILTERED_SIZE) |
					  CAN_RXF1C_F1OM;								/* Overwrite mode, no need to process these messages */

	/* Configure the CAN controller with the max. number of bytes to capture from the payload of each Rx message */
	/* Element data size must match the CanMramRxf0e/Rxf1e structs used to walk
	   these FIFOs, which carry a 64-byte data array - 72 bytes per element.
	   Configured for DATA8 the peripheral packs elements every 16 bytes, so
	   indexing the array found the right element only at index 0 and garbage
	   for every one after it. */
	CAN0->RXESC.reg = CAN_RXESC_RBDS_DATA64 | CAN_RXESC_F0DS_DATA64 | CAN_RXESC_F1DS_DATA64;

	/* Disable CAN Tx Buffer Tx interrupts */
	CAN0->TXBTIE.reg = 0U;

	/* Tx Buffer & Queue/FIFO Configuration */
	CAN0->TXBC.reg = CAN_TXBC_NDTB(CAN_TX_BUFFER_SIZE) | CAN_TXBC_TFQS(CAN_TX_QUEUE_SIZE) | CAN_TXBC_TFQM | CAN_TXBC_TBSA((uint32_t)CAN_TxBuffer);

	/* Configure the CAN controller with the code for the max. payload of each Tx message using a Tx Buffer Element */
	CAN0->TXESC.reg = CAN_TXESC_TBDS_DATA64;

	/* Clear all interrupt flags */
	CAN0->IR.reg = CAN_IR_MASK;	/* Clear all interrupt flags */

	/* Interrupt Line Select */
	CAN0->ILS.reg = 0;	/* All interrupts default to line zero */

	/* Interrupt Line Enable ZERO */
	CAN0->ILE.reg = CAN_ILE_EINT0;

	/* Enable CAN interrupts,  RFxNE: Rx FIFO x New Message Interrupt Enable */
	CAN0->IE.reg = CAN_IE_RF1NE | CAN_IE_RF0NE;
				   
	/* Enable the NVIC line.  CAN0->IE/ILE enable the interrupt inside the
	   peripheral, but without this CAN0_Handler is never entered and
	   nothing drains FIFO 0. */
	NVIC_SetPriority(CAN0_IRQn, 2);
	NVIC_EnableIRQ(CAN0_IRQn);

	/* Enable CAN */
	CAN0->CCCR.reg = 0U; /* Enable CAN */
}


/* Transmit with an 11-bit standard identifier.  The M_CAN Tx element holds
   the identifier left-aligned in a 29-bit field, so a standard ID goes in
   bits 28:18 and XTD stays clear - getting this wrong is silent, the frame
   simply goes out with a nonsense identifier. */
uint32_t CAN_TxDropped;
uint32_t CAN_BusOffRecoveries;

/* Recover the controller from bus-off.

   When the transmit error counter passes 255 the CAN controller takes itself
   bus-off: the hardware sets CCCR.INIT and the node stops transmitting and
   receiving entirely. It does NOT come back on its own - INIT stays set until
   software clears it, and nothing in this firmware used to. A quiet bus was
   therefore permanently fatal to telemetry, recoverable only by a power cycle
   or a debugger, which is exactly what kept happening on the bench.

   Getting there is quick: the error counter rises by 8 per unacknowledged
   frame, so at the 250 frames/s this board transmits, about 30 frames - a
   little over a tenth of a second - is enough if nothing else is on the bus.

   Clearing INIT starts the recovery sequence defined by the CAN standard: the
   node waits to observe 129 occurrences of 11 consecutive recessive bits
   before it rejoins, which on an idle 500 kbit/s bus takes a few milliseconds.
   PSR.BO stays set for the duration and clears when recovery completes, so
   testing INIT as well as BO means this kicks recovery once and then leaves it
   alone rather than restarting it on every call.

   No rate limiting is needed beyond that: this is called from the telemetry
   task's tick, so a bus that keeps failing simply retries at the tick rate
   rather than spinning. CAN_BusOffRecoveries counts how often it has happened,
   which is what turns "telemetry stopped" into a diagnosis. */
void CAN_Poll(void)
{
	if (CAN0->PSR.bit.BO && CAN0->CCCR.bit.INIT)
	{
		CAN0->CCCR.reg &= ~CAN_CCCR_INIT;
		CAN_BusOffRecoveries += 1;
	}
}



/* How long to wait for a free Tx queue slot before giving up.
   
   Deliberately a spin count rather than a time: this is the transmit path and
   must stay safe to call from an interrupt, where a SysTick-derived timeout
   could never expire. The exact duration does not matter, only that it is
   bounded and comfortably longer than one frame - a maximum length standard
   frame at 500 kbit/s is about 130 bit times, roughly 260us, and each pass of
   the loop is an APB read of several cycles at 48MHz. This gives order
   milliseconds, so a slot that is merely momentarily busy is still waited for.

   The point is the bound, not the value. An unbounded wait here is what wedged
   the board repeatedly: with nothing acknowledging on the bus the eight deep
   queue fills within about 30 frames, and the caller then spins forever - which
   in the old firmware was an interrupt handler, taking the SDL sniffer and all
   telemetry down with it. Dropping a periodic telemetry frame costs nothing;
   the next one is along in 20ms. */
#define CAN_TX_WAIT_SPINS			(50000UL)

static bool CAN_TxWaitForSlot(void)
{
	uint32_t Spins = CAN_TX_WAIT_SPINS;

	while (CAN0->TXFQS.bit.TFQF)
	{
		if (Spins == 0)
		{
			CAN_TxDropped += 1;
			return false;
		}
		Spins -= 1;
	}

	return true;
}


bool CAN_TxStandard(uint16_t Id, const void *Data, uint32_t DataSize)
{
	if (!CAN_TxWaitForSlot())
		return false;

	uint8_t Index = CAN0->TXFQS.bit.TFQPI;
	CanMramTxbe *TxBufferElement = &CAN_TxBuffer[Index];
	TxBufferElement->TXBE_0.reg = CAN_TXBE_0_ID((uint32_t)Id << 18);
	TxBufferElement->TXBE_1.reg = CAN_TXBE_1_DLC(DataSize);
	memcpy((void *)&TxBufferElement->TXBE_DATA, Data, DataSize);
	CAN0->TXBAR.reg = 1UL << Index;

	return true;
}


/* Transmit with a 29-bit extended identifier. */
bool CAN_Tx(uint32_t Id, const void *Data, uint32_t DataSize)
{
	if (!CAN_TxWaitForSlot())
		return false;

	uint8_t Index = CAN0->TXFQS.bit.TFQPI;
	CanMramTxbe *TxBufferElement = &CAN_TxBuffer[Index];
	TxBufferElement->TXBE_0.reg = CAN_TXBE_0_XTD | CAN_TXBE_0_ID(Id);
	TxBufferElement->TXBE_1.reg = CAN_TXBE_1_DLC(DataSize);	
	memcpy((void *)&TxBufferElement->TXBE_DATA, Data, DataSize);
	CAN0->TXBAR.reg = 1UL << Index;

	return true;
}

/* Drain Rx FIFO 0 and hand each accepted frame to the registered handler.
   FIFO 1 needs no draining - it is in overwrite mode and exists only so the
   errata workaround sees an interrupt per bus message - but its flag still
   has to be cleared or the line would stay asserted. */
void CAN0_Handler(void)
{
	const uint32_t Status = CAN0->IR.reg;
	CAN0->IR.reg = Status;

	if (Status & CAN_IR_RF0N)
	{
		while (CAN0->RXF0S.bit.F0FL)
		{
			const uint8_t Index = CAN0->RXF0S.bit.F0GI;
			const CanMramRxf0e *Element = &CAN_RxFifo0Filtered[Index];

			/* Standard identifiers sit in bits 28:18, as on the transmit side */
			const uint16_t Id = (uint16_t)((Element->RXF0E_0.reg >> 18) & 0x7FF);
			const uint8_t Length = (uint8_t)((Element->RXF0E_1.reg >> 16) & 0x0F);

			if (CAN_RxHandler)
				CAN_RxHandler(Id, (const uint8_t *)&Element->RXF0E_DATA[0], Length);

			/* Acknowledge, releasing the element back to the FIFO */
			CAN0->RXF0A.reg = CAN_RXF0A_F0AI(Index);
		}
	}
}
