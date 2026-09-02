#include "os.h"
#include "rtime.h"
#include "diag.h"
#include "pio.h"
#include "sercom.h"
#include "debug.h"
#include "mem.h"
#include "os_task_id.h"
//#include "messages.h"

#define DIAG_SIGNAL_WRITE	(OS_SIGNAL_MESSAGE << 1)
#define DIAG_SIGNAL_READ	(OS_SIGNAL_MESSAGE << 2)
#define DIAG_SIGNAL_TICK	(OS_SIGNAL_MESSAGE << 3)

#define DIAG_STATE_WAIT_CMD		// Wait for 0xDA or ADC channel

#define DIAG_STATE_ADC_MSB_DATA
#define DIAG_STATE_ADC_LSB_CMD
#define DIAG_STATE_ADC_LSB_DATA

#define BUILD_DIAG_WRITE

#define DIAG_CMD_READ_16		(0xDA)
#define DIAG_CMD_WRITE_16_ADDR	(0xDB)
#define DIAG_CMD_WRITE_16_DATA	(0xDC)
#define DIAG_CMD_WRITE_8_ADDR	(0xDD)
#define DIAG_CMD_WRITE_8_DATA	(0xDE)

Diag_t Diag;


#define DIAG_READ_IDLE		(0) /*  */
#define DIAG_READ_READY		(2)	/* Ready to read data */
#define DIAG_READ_AVAILABLE	(1)	/* Data has been read */


uint32_t Diag_TimeMs;

/* Last completed scheduled read, and a count of them. Debug() is compiled
   out, so without these a working read leaves no evidence at all. Readable
   over SWD, and the values the CAN response frame will carry. */
uint32_t Diag_ReadCount;
uint16_t Diag_LastAddress;
uint16_t Diag_LastValue;

/* Called when a scheduled read completes.  Lets the CAN command layer emit
   a response without diag.c knowing anything about CAN. */
static Diag_ReadComplete_t Diag_ReadCompleteHandler;

void Diag_SetReadCompleteHandler(Diag_ReadComplete_t Handler)
{
	Diag_ReadCompleteHandler = Handler;
}

static Diag_WriteComplete_t Diag_WriteCompleteHandler;

void Diag_SetWriteCompleteHandler(Diag_WriteComplete_t Handler)
{
	Diag_WriteCompleteHandler = Handler;
}


/***************************************************************************************/
uint16_t Diag_Time(void)
{
	return Diag_TimeMs;
}


void Diag_TimerTick(Diag_t *Diag)
{
	Diag_TimeMs++;

	/* Wake the task only when the head of the list has actually come due,
	   rather than on every one of the 1000 ticks a second. From SysTick. */
	if (Diag->ReadState == DIAG_READ_IDLE && Diag->ReadList &&
	    Time_Le(Diag->ReadList->Time, Diag_Time()))
		OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_TICK);
}

/***************************************************************************************/
void Diag_ReadEntryInsert(Diag_t * Diag, Diag_ReadEntry_t *EntryNew)
{
	Diag_ReadEntry_t **EntryRef, *Entry;
	const time_t Time = EntryNew->Time;
	
	for (EntryRef = &Diag->ReadList; (Entry = *EntryRef) != NULL; EntryRef = &Entry->Next)
		if (Time_Lt(Time, Entry->Time))
			break;

    EntryNew->Next = Entry;
	*EntryRef = EntryNew;
}


/***************************************************************************************/
Diag_ReadEntry_t *Diag_ReadEntryRemoveHead(Diag_t *Diag)
{
	if (Diag->ReadList != NULL)
	{
		Diag_ReadEntry_t *Entry = Diag->ReadList;
		Diag->ReadList = Diag->ReadList->Next;
		Entry->Next = NULL;
		return Entry;
	}		
	else
		return NULL;
}


/***************************************************************************************/
/* Send 16-bit UART data */
static void Diag_TxData16(Diag_t *Diag, const uint16_t TxData)
{
	/* Enable UART Tx */
	SERCOM_UsartTxEnable(Diag->Hw.Usart);

	/* Load transmit buffer */
	while (!SERCOM_UsartTxReady(Diag->Hw.Usart));
	Diag->Hw.Usart->DATA.reg = TxData >> 8;
	while (!SERCOM_UsartTxReady(Diag->Hw.Usart));
	Diag->Hw.Usart->DATA.reg = TxData & 0xFF;

	/* Disable UART Tx */
	SERCOM_UsartTxDisable(Diag->Hw.Usart);
}


/***************************************************************************************/
static void Diag_TxCommand(Diag_t *Diag, uint8_t Command)
{
	Diag_TxData16(Diag, 0x100 | Command);
}


static void Diag_StartTimer(Diag_t *Diag)
{
}

static void Diag_CancelTimer(Diag_t *Diag)
{
}


/***************************************************************************************/

static void Diag_RxHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxCommandMsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxCommandLsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxReadDataMsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxReadDataLsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxWriteDataAckMsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxWriteDataAckLsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxWriteAddressAckMsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxWriteAddressAckLsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxAdcMsbHandler(Diag_t *Diag, uint16_t RxData);
static void Diag_RxAdcLsbHandler(Diag_t *Diag, uint16_t RxData);

static void Diag_RxCommandMsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag->RxHandler = Diag_RxCommandLsbHandler;
}

static void Diag_RxCommandLsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag->RxHandler = Diag_RxHandler;
}

static void Diag_RxReadDataMsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag->ReadData[0] = (uint8_t)RxData;	
	Diag->RxHandler = Diag_RxReadDataLsbHandler;
}

static void Diag_RxReadDataLsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag_CancelTimer(Diag);

	Diag->ReadData[1] = (uint8_t)RxData;
	Diag->RxHandler = Diag_RxHandler;

	/* Set state to indicate data available */
	Diag->ReadState = DIAG_READ_AVAILABLE;

	/* Wake up task to handle read data */
	OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_READ);
}

static void Diag_RxWriteDataAckMsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag->WriteDataAck = RxData << 8;
	Diag->RxHandler = Diag_RxWriteDataAckLsbHandler;
}

static void Diag_RxWriteDataAckLsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag_CancelTimer(Diag);

	Diag->WriteDataAck |= RxData;

	/* As above - restore the dispatcher. */
	Diag->RxHandler = Diag_RxHandler;
	if (Diag->WriteDataAck == Diag->WriteData)
	{
		/* Set flag to update write in background */
		OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_WRITE);
		//Diag->WriteDataUpdate = 1;
			
		/* Clear ready flag, nothing more to write */
		Diag->WriteDataReady = 0;

		if (Diag->WriteSize == 0 && Diag_WriteCompleteHandler)
			Diag_WriteCompleteHandler(true);
	}
	else
	{
		/* TODO: Retry write data */
	}
}

static void Diag_RxWriteAddressAckMsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag->WriteAddressAck = RxData << 8;
	Diag->RxHandler = Diag_RxWriteAddressAckLsbHandler;
}

static void Diag_RxWriteAddressAckLsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag_CancelTimer(Diag);

	Diag->WriteAddressAck |= RxData;

	/* Hand the dispatcher back.  The read path does this and the write path
	   did not, so every frame after a write kept landing in this handler
	   instead of Diag_RxHandler. */
	Diag->RxHandler = Diag_RxHandler;
	if (Diag->WriteAddressAck == Diag->WriteAddress)
	{
		/* Clear ready flag, address has been written */
		Diag->WriteAddressReady = 0;

		/* Wake the task to prepare the data phase.  Without this the write
		   stopped here: the address was acknowledged and nothing ever set
		   WriteDataReady, so the data was never sent. */
		OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_WRITE);
	}
	else
	{
		/* TODO: Retry write address */
	}

}

static void Diag_RxAdcMsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag->RxHandler = Diag_RxAdcLsbHandler;
	Diag->AdcData[Diag->AdcChannel] = (RxData & 0xFF) << 8;
}

static void Diag_RxAdcLsbHandler(Diag_t *Diag, uint16_t RxData)
{
	Diag->AdcData[Diag->AdcChannel] |= (RxData & 0xFF);
	Diag->RxHandler = Diag_RxHandler;

	/* Switch USART Rx to back PAD 2 to receive data from MCU */
	SERCOM_UsartSetRxPad(Diag->Hw.Usart, 2);

	//Debug("ADC %x %04X ", Diag->AdcChannel, Diag->AdcData[Diag->AdcChannel] >> 4);
}


static void Diag_ReadData(Diag_t *Diag, uint16_t RxData)
{
	Diag_StartTimer(Diag);

	/* Check ECU is in the correct state already */
	if (RxData == DIAG_CMD_READ_16)
	{
		/* ECU is in Read Mode, so transmit address of data required */
		Diag_TxData16(Diag, Diag->ReadAddress);
		Diag->RxHandler = Diag_RxReadDataMsbHandler;
	}
	else
	{
		/* Incorrect state, request read mode */
		Diag_TxCommand(Diag, DIAG_CMD_READ_16);
		Diag->RxHandler = Diag_RxCommandMsbHandler;
	}
}

static void Diag_WriteData(Diag_t *Diag, uint16_t RxData)
{
	Diag_StartTimer(Diag);

	/* Check ECU is in the correct state already */
	if (RxData == Diag->WriteDataCommand)
	{
		/* Transmit data to write */
		Diag_TxData16(Diag, Diag->WriteData);
		Diag->RxHandler = Diag_RxWriteDataAckMsbHandler;
	}
	else
	{
		/* Incorrect state, request write data mode */
		Diag_TxCommand(Diag, Diag->WriteDataCommand);
		Diag->RxHandler = Diag_RxCommandMsbHandler;
	}
}

static void Diag_WriteAddress(Diag_t *Diag, uint16_t RxData)
{
	Diag_StartTimer(Diag);

	/* Check ECU is in the correct state already */
	if (RxData == Diag->WriteAddressCommand)
	{
		/* Transmit address of write */
		Diag_TxData16(Diag, Diag->WriteAddress);
		Diag->RxHandler = Diag_RxWriteAddressAckMsbHandler;
	}
	else
	{
		/* Incorrect state, request write address mode */
		Diag_TxCommand(Diag, Diag->WriteAddressCommand);
		Diag->RxHandler = Diag_RxCommandMsbHandler;
	}
}

static void Diag_RxHandler(Diag_t *Diag, uint16_t RxData)
{
	if (DIAG_IsCommand(RxData))
	{
		if (Diag->ReadState == DIAG_READ_READY)
			Diag_ReadData(Diag, RxData);
#ifdef BUILD_DIAG_WRITE
		else if (Diag->WriteDataReady)
			Diag_WriteData(Diag, RxData);
		else if (Diag->WriteAddressReady)
			Diag_WriteAddress(Diag, RxData);
#endif
		//Debug("%02X\n", RxData);
	}
	else
	{
		if (RxData != 0x102)
		{
			/* Must be ADC channel request */
			Diag->AdcChannel = (RxData >> 1) & 0x1F;
			Diag->RxHandler = Diag_RxAdcMsbHandler;

			/* Switch USART Rx to PAD 0 to receive data from ADC */
			SERCOM_UsartSetRxPad(Diag->Hw.Usart, 0);

			//Debug("%02X:", Diag->AdcChannel);
		}
	}
}


#if 0
static void Diag_HandleEnableRequest(Diag_t *Diag, Diag_Msg_Enable_Req_t *Msg)
{
	Msg->Id = DIAG_MSG_ENABLE_CFM_ID;
	OS_MessageSend(Msg, Msg->Sender);
}

static void Diag_HandleDisableRequest(Diag_t *Diag, Diag_Msg_Disable_Req_t *Msg)
{
	Msg->Id = DIAG_MSG_DISABLE_CFM_ID;
	OS_MessageSend(Msg, Msg->Sender);
}

static void Diag_HandleAddRequest(Diag_t *Diag, Diag_Msg_Add_Req_t *Req)
{
	Diag_ReadEntry_t *Entry = MEM_Create(Diag_ReadEntry_t);
	if (Entry)
	{
		Entry->Address = Req->Address;
		Entry->Period = Req->Period;
		Entry->Repeat = 0;
		Entry->Size = Req->Size;
		Entry->Time = Diag_Time() + Entry.Period;
		Diag_ReadEntryInsert(Diag, Entry);
	}

	Diag_Msg_Add_Cfm_t *Cfm = MEM_Create(Diag_Msg_Add_Cfm_t);
	Cfm->Header.Id = DIAG_MSG_ADD_CFM_ID;

	OS_MessageSend(Cfm, Req->Header.Sender);
	MEM_Free(Req);
}
#endif

static void Diag_HandleMessage(Diag_t *Diag, OS_Message_t *Msg)
{
#if 0
	switch (Msg->Id)
	{
		case DIAG_MSG_ENABLE_REQ_ID:
			Diag_HandleEnableRequest(Diag, Msg);
			break;

		case DIAG_MSG_DISABLE_REQ_ID:
			Diag_HandleDisableRequest(Diag, Msg);
			break;
			
		case DIAG_MSG_ADD_REQ_ID:
			Diag_HandleAddRequest(Diag, Msg);
			break;
		
		case DIAG_MSG_REMOVE_REQ_ID:
			Diag_HandleRemoveRequest(Diag, Msg);
			break;
	}
#endif

	MEM_Free(Msg);
}


/***************************************************************************************/
void SERCOM0_Handler(void)  __attribute__((__interrupt__));
void SERCOM0_Handler(void)
{
	SercomUsart *Usart = Diag.Hw.Usart;
	uint8_t IntFlag = Usart->INTFLAG.reg;

	/* Clear interrupts */
	Usart->INTFLAG.reg = IntFlag;

	/* Check if there's UART Rx data */
	if (IntFlag & SERCOM_USART_INTFLAG_RXC)
	{
		const uint16_t RxData = Usart->DATA.reg;

		/* Check UART reception was error free */		
		if (!Usart->INTFLAG.bit.ERROR)
		{
			/* Call receive handler */
			Diag.RxHandler(&Diag, RxData);
		}
	}
}






void Diag_Task(void *Context)
{
	Diag_t *Diag = (Diag_t *)Context;

	Diag->Instance = 0;
	Diag->Hw.Usart = &SERCOM_GetSercom(Diag->Instance)->USART;

	/* Set default receive handler */
	Diag->RxHandler = Diag_RxHandler;

	/* Enable USART clock in PMC */
	SERCOM_EnableClock(Diag->Instance);

	/* Configure PIOs for this USART */
	SERCOM_ConfigurePios(Diag->Instance, 7);

	/* Configure USART for 1MBaud, 9N1, synchronous, external clock */
	SERCOM_UsartInit(Diag->Instance,
		SERCOM_USART_CTRLA_CMODE | SERCOM_USART_CTRLA_DORD | SERCOM_USART_CTRLA_CPOL | SERCOM_USART_CTRLA_RXPO(2) | SERCOM_USART_CTRLA_TXPO(0) | SERCOM_USART_CTRLA_RUNSTDBY,
		SERCOM_USART_CTRLB_RXEN | SERCOM_USART_CTRLB_CHSIZE(1),	0x0000,	0);

	/* Enable USART */
	SERCOM_UsartEnable(Diag->Hw.Usart);

	/* Enable USART interrupts */
	Diag->Hw.Usart->INTENSET.reg = SERCOM_USART_INTENSET_RXC;
	NVIC_SetPriority(SERCOM_GetIrqNumber(Diag->Instance), 1);
	NVIC_EnableIRQ(SERCOM_GetIrqNumber(Diag->Instance));


	for (;;)
	{
		OS_SignalSet_t Signals = OS_SignalWait(OS_SIGNAL_MESSAGE | DIAG_SIGNAL_READ | DIAG_SIGNAL_WRITE | DIAG_SIGNAL_TICK);

		/* Check if any messages to be handled */
		if (Signals & OS_SIGNAL_MESSAGE)
		{
			OS_Message_t *Msg;
			while ((Msg = OS_MessageGet()) != NULL)
				Diag_HandleMessage(Diag, Msg);
		}

#ifdef BUILD_DIAG_WRITE
		/* Check if write update required */
		if (Signals & DIAG_SIGNAL_WRITE)
		{
			const uint8_t WriteSize = Diag->WriteSize > 2 ? 2 : Diag->WriteSize;

			if (WriteSize > 0)
				Diag->WriteData = Diag->WriteBuffer[Diag->WriteIndex + 0] << 8;
			if (WriteSize > 1)
				Diag->WriteData |= Diag->WriteBuffer[Diag->WriteIndex + 1];
					
			Diag->WriteIndex += WriteSize;
			Diag->WriteAddress += WriteSize;
			Diag->WriteSize -= WriteSize;

			Diag->WriteDataCommand = (WriteSize > 1) ? DIAG_CMD_WRITE_16_DATA : DIAG_CMD_WRITE_8_DATA;
			/* Ready if a chunk was just prepared - NOT if bytes remain after it,
			   which is what this used to test.  For a single two-byte write the
			   remainder is zero, so the data phase never started. */
			Diag->WriteDataReady   = (WriteSize > 0);	
		}
#endif
	
		/* Check if read data is available from interrupt */
		if (Signals & DIAG_SIGNAL_READ)
		{
			const uint8_t ReadSize = Diag->ReadSize > 2 ? 2 : Diag->ReadSize;

			/* Copy data into buffer */		
			if (ReadSize > 0)
				Diag->ReadCurrent->Buffer[Diag->ReadIndex + 0] = Diag->ReadData[0];
			if (ReadSize  > 1)
				Diag->ReadCurrent->Buffer[Diag->ReadIndex + 1] = Diag->ReadData[1];
		
			/* Update index, address and size */
			Diag->ReadIndex += ReadSize;
			Diag->ReadAddress += ReadSize;
			Diag->ReadSize -= ReadSize;

			/* Check if there are still bytes to read */
			if (Diag->ReadSize)
			{
				/* Move back to ready state so that interrupt will read more data */
				Diag->ReadState = DIAG_READ_READY;
			}
			else
			{
				/* TODO: Check if repeat read */
				if (Diag->ReadRepeat)
				{
				
				}
				else
				{
					/* Move back to idle state, another read needs to be scheduled */
					Diag->ReadState = DIAG_READ_IDLE;
			
					/* TODO: Send result over UART */
					Diag_LastAddress = Diag->ReadCurrent->Address;
					Diag_LastValue = (uint16_t)((Diag->ReadData[0] << 8) | Diag->ReadData[1]);
					Diag_ReadCount += 1;

					if (Diag_ReadCompleteHandler)
						Diag_ReadCompleteHandler(Diag->ReadCurrent, Diag_LastValue);
			
					/* A Period of zero is a one-shot read: do not re-queue it.  The
					   completion handler above owns it now and frees it. */
					if (Diag->ReadCurrent->Period != 0)
					{
					/* Update the current read entry time for next period */
					Diag->ReadCurrent->Time = Time_Add(Diag->ReadCurrent->Time, Diag->ReadCurrent->Period);
					time_t Time = Diag_Time();
					if (Time_Le(Diag->ReadCurrent->Time, Time))
						Diag->ReadCurrent->Time = Time;
			
					/* Insert read entry back into linked list */
					Diag_ReadEntryInsert(Diag, Diag->ReadCurrent);
					}
				}
			}		
	
		}

		/* Start the next scheduled read if one has come due.  Deliberately outside
		   the DIAG_SIGNAL_READ branch: that signal is only raised when a read
		   COMPLETES, so with the check in there nothing ever started the first
		   read, and once one finished with the next not yet due the task slept
		   with nothing left to wake it. Diag_TimerTick now raises
		   DIAG_SIGNAL_TICK when the head of the list comes due. */
		if (Diag->ReadState == DIAG_READ_IDLE && Diag->ReadList)
		{
			time_t Time = Diag_Time();

			if (Time_Le(Diag->ReadList->Time, Time))
			{
				Diag->ReadCurrent = Diag_ReadEntryRemoveHead(Diag);

				Diag->ReadAddress = Diag->ReadCurrent->Address;
				Diag->ReadSize = Diag->ReadCurrent->Size;
				Diag->ReadIndex = 0;

				/* Ready state, so the receive interrupt starts the transfer */
				Diag->ReadState = DIAG_READ_READY;
			}
		}
	}
}


void Diag_Init(void)
{
	/* No reads are registered here any more.  Two hard-coded debug entries
	   used to live here; now that a completed read emits a CAN response they
	   would be unsolicited traffic.  Reads are requested over CAN - see
	   diag_can.c. */
	OS_TaskInit(DIAG_TASK_ID, Diag_Task, &Diag, Diag.Stack, sizeof(Diag.Stack));
}
