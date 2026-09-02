/*
 * can.c
 *
 * Created: 23/05/2023 16:45:33
 *  Author: jonso
 */ 

#include <sam.h>
#include "pio.h"
#include <stdalign.h>
#include <string.h>

#include "can.h"

#define CAN_FILTER_ID         0x45a

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


//static alignas(4) CanMramSidfe can_rx_standard_filter;

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
	/* Request Stop CAN comms, start initialization */
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

	/* Rx FIFO 0 - this could be used for receiving messages to be processed */
	CAN0->RXF0C.reg = CAN_RXF0C_F0SA((uint32_t)CAN_RxFifo0Filtered) | CAN_RXF0C_F0S(CAN_RX_FIFO_FILTERED_SIZE);

	/* Rx FIFO 1 Config */
	/* FIFO 1 is used as a dumping ground for all messages that are not processed
		but accepted by the Global Filter Configuration in order to receive interrupts
		at the end of every bus message for errata 1.6.18 Workaround 1 */
	CAN0->RXF1C.reg = CAN_RXF1C_F1SA((uint32_t)CAN_RxFifo1Unfiltered) | CAN_RXF1C_F1S(CAN_RX_FIFO_UNFILTERED_SIZE) |
					  CAN_RXF1C_F1OM;								/* Overwrite mode, no need to process these messages */

	/* Configure the CAN controller with the max. number of bytes to capture from the payload of each Rx message */
	CAN0->RXESC.reg = CAN_RXESC_RBDS_DATA8 | CAN_RXESC_F0DS_DATA8 | CAN_RXESC_F1DS_DATA8;


	/* Disable CAN Tx Buffer Tx interrupts */
	CAN0->TXBTIE.reg = 0U;

	/* Tx Buffer & Queue/FIFO Configuration */
	CAN0->TXBC.reg = CAN_TXBC_NDTB(CAN_TX_BUFFER_SIZE) | CAN_TXBC_TFQS(CAN_TX_QUEUE_SIZE) | CAN_TXBC_TFQM | CAN_TXBC_TBSA((uint32_t)CAN_TxBuffer);
	//CAN0->TXEFC.reg  = CAN_TXEFC_EFSA((uint32_t)&can_tx_event_fifo) | CAN_TXEFC_EFS(1);

	/* Configure the CAN controller with the code for the max. payload of each Tx message using a Tx Buffer Element */
	CAN0->TXESC.reg = CAN_TXESC_TBDS_DATA64;
				   
	//CAN0->SIDFC.reg  = CAN_SIDFC_FLSSA((uint32_t)&can_rx_standard_filter) | CAN_SIDFC_LSS(1);
	//CAN0->XIDFC.reg  = 0;
	//CAN0->TSCC.reg   = CAN_TSCC_TCP(0) | CAN_TSCC_TSS_INC_Val;
	//CAN0->TDCR.reg   = CAN_TDCR_TDCO(0) | CAN_TDCR_TDCF(0);
	//CAN0->GFC.reg    = CAN_GFC_ANFS_REJECT | CAN_GFC_ANFE_REJECT | CAN_GFC_RRFS | CAN_GFC_RRFE;
	//CAN0->XIDAM.reg  = CAN_XIDAM_MASK;
	//CAN0->TXBTIE.reg = CAN_TXBTIE_MASK;
	//CAN0->TXBCIE.reg = CAN_TXBCIE_MASK;

	/* Clear all interrupt flags */
	CAN0->IR.reg = CAN_IR_MASK;	/* Clear all interrupt flags */

	/* Interrupt Line Select */
	CAN0->ILS.reg = 0;	/* All interrupts default to line zero */

	/* Interrupt Line Enable ZERO */
	CAN0->ILE.reg = CAN_ILE_EINT0;

	/* Enable CAN interrupts,  RFxNE: Rx FIFO x New Message Interrupt Enable */
	CAN0->IE.reg = CAN_IE_RF1NE | CAN_IE_RF0NE;

	//NVIC_EnableIRQ(CAN0_IRQn);	/* Enable interrupts in the int Controller */

	/* Enable CAN */
	CAN0->CCCR.reg = 0U; /* Enable CAN */


//	can_rx_standard_filter.SIDFE_0.reg = CAN_SIDFE_0_SFID2(0) | CAN_SIDFE_0_SFID1(CAN_FILTER_ID) |
//										 CAN_SIDFE_0_SFT_CLASSIC | CAN_SIDFE_0_SFEC_STRXBUF;	
}

#define TX_END_FLAGS (CAN_IR_TC | CAN_IR_TOO | CAN_IR_PEA | CAN_IR_PED)

void CAN_Tx(uint32_t Id, void *Data, uint32_t DataSize)
{
	/* Wait if queue is full */
	while (CAN0->TXFQS.bit.TFQF);

	uint8_t Index = CAN0->TXFQS.bit.TFQPI;
	CanMramTxbe *TxBufferElement = &CAN_TxBuffer[Index];
	TxBufferElement->TXBE_0.reg = CAN_TXBE_0_XTD | CAN_TXBE_0_ID(Id);
	TxBufferElement->TXBE_1.reg = CAN_TXBE_1_DLC(DataSize);	
	memcpy((void *)&TxBufferElement->TXBE_DATA, Data, DataSize);
	CAN0->TXBAR.reg = 1UL << Index;
}