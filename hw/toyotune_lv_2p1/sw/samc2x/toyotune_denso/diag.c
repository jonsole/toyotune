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

/* TEMPORARY BENCH EXPERIMENT - set back to 1 to restore ADC snooping.

   SERCOM_UsartSetRxPad() does not merely retarget the pad: it calls
   SERCOM_UsartDisable()/Enable(), switching the whole USART off and on with a
   SYNCBUSY.ENABLE spin each way.  The receiver is dead for that window, so any
   frame arriving is lost silently - no interrupt, no error flag.  Losing one
   frame of the ECU's echo leaves a read/write sequence armed and one frame out
   of step, which then captures the ECU's ADC traffic (0x101 then 0x102) as the
   result.  This switch tests that theory by never touching the pad: foreign
   (bit-8-set) frames are simply ignored and the dispatcher stays in control.
   Cost is AdcData[] never being populated, which nothing reads - its only
   consumer is a Debug() that debug.h compiles out. */
#define DIAG_ADC_SNOOP    (0)

#define DIAG_CMD_READ_16		(0xDA)

Diag_t Diag;

/* Spin bound for waiting on TXC.  The ECU is the clock master, so a link that
   stops clocking must not wedge the SERCOM0 ISR - count the give-ups instead. */
#define DIAG_TX_DRAIN_GUARD	(2000)
volatile uint32_t Diag_TxDrainTimeouts;


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

	/* A completed read the task has not picked up yet.  Re-raise the signal
	   until it does: the ISR raises DIAG_SIGNAL_READ once, and if that wakeup
	   is missed the link wedges permanently, because the TICK below is gated
	   on DIAG_READ_IDLE and so can never fire while we sit in AVAILABLE.
	   Seen on the bench after ~3400 good reads: mode stuck at DAh, ReadState
	   stuck at AVAILABLE, the diag_can entry pool full, and every later read
	   answered with an error.  Cheap to re-send - the task clears the state
	   as soon as it runs. */
	if (Diag->ReadState == DIAG_READ_AVAILABLE)
		OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_READ);

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
/* ==== Protocol v2 - one inbound byte per poll ==============================

   The D8X receive buffer holds ONE byte, and its diag routine runs inside
   int_vector_1_serial_rx, which the NE crank interrupt preempts for ~350us.
   v1 answered the ECU's sync with two bytes 11us apart, so an NE landing
   between them overran the first: ~10% of reads lost at 7000 rpm.  That was
   never fixable from this side, which is why nothing tried here ever helped.

   v2: send exactly ONE byte per poll, so an overrun needs two bytes in flight
   and cannot happen.  The ECU never waits either, so it no longer blocks its
   own ISR - better for engine timing than the code this replaces, not worse.

   Framing.  The ECU announces its state as a MARK-parity byte in D0..DF, then
   sends that state's payload as SPACE-parity bytes.  Its ADC traffic is mark
   with values 00..1F, so there is no overlap and every frame is classifiable
   with no state of our own - which is what makes this resynchronising: lose a
   byte and the next announcement says exactly where the ECU is.

   We answer once per group, on its LAST frame, so the decision is taken with
   the payload already in hand.                                             */

/* Scope trigger.  Goes HIGH when we answer the ECU's idle announcement and
   start a transaction, LOW when that transaction completes - so the pulse
   spans the whole exchange and its width is the transfer time.  On PA23 =
   X2.3, a spare pin on the CAN connector, with ground on X2.6.  Set to 0 to
   compile it out. */
#define DIAG_SCOPE_MARK           (1)
#define DIAG_SCOPE_PIN            PIN_PA23

#if DIAG_SCOPE_MARK
#define DIAG_ScopeHigh()          PIO_Set(DIAG_SCOPE_PIN)
#define DIAG_ScopeLow()           PIO_Clear(DIAG_SCOPE_PIN)
#else
#define DIAG_ScopeHigh()          do { } while (0)
#define DIAG_ScopeLow()           do { } while (0)
#endif

#define DIAG_MODE_READ_ADDR_MSB   (0xDA)  /* idle; expecting read address MSB */
#define DIAG_MODE_READ_ADDR_LSB   (0xD0)
#define DIAG_MODE_READ_RESULT     (0xD8)  /* 2 payload bytes; auto-increments */
/* Two write phases, picked by Diag_t.WriteWidth.  Both are atomic in the ECU:
   the 16-bit one holds the MSB and commits with a single st d,[y], the 8-bit
   one commits immediately with a single st a,[y].  A byte write is NOT a
   read-modify-write and does not need emulating with a word write - which is
   exactly why it is worth offering: writing one byte of a pair leaves its
   neighbour alone, where a word write would have to read and rewrite it. */
#define DIAG_MODE_WRITE_ADDR_MSB  (0xDD)  /* 16-bit write phase */
#define DIAG_MODE_WRITE_ADDR_LSB  (0xD3)
#define DIAG_MODE_WRITE_DATA      (0xDE)  /* expecting the data MSB */
#define DIAG_MODE_WRITE_DATA_LSB  (0xD4)  /* expecting the data LSB */
#define DIAG_MODE_WRITE_ACK       (0xDF)  /* 2 payload bytes: the read-back */
#define DIAG_MODE_WRITE8_ADDR_MSB (0xDC)  /* 8-bit write phase */
#define DIAG_MODE_WRITE8_ADDR_LSB (0xD5)
#define DIAG_MODE_WRITE8_DATA     (0xD6)  /* expecting the data byte */
#define DIAG_MODE_WRITE8_ACK      (0xD7)  /* 1 payload byte: the read-back */

static uint8_t Diag_Mode;          /* last state the ECU announced */
static uint8_t Diag_PayloadLeft;   /* payload frames still expected */
static uint8_t Diag_PayloadIndex;
static uint8_t Diag_Payload[2];

uint32_t Diag_ProtocolResyncs;     /* announcements we could not account for */
uint32_t Diag_ReadStrays;          /* streamed words arriving with no read pending */

/* Send one frame.  Mark parity marks a command, space a data byte - the ECU
   tests SSD.0 to tell them apart. */
static void Diag_TxFrame(Diag_t *Diag, uint16_t Frame)
{
	SERCOM_UsartTxEnable(Diag->Hw.Usart);

	while (!SERCOM_UsartTxReady(Diag->Hw.Usart));
	Diag->Hw.Usart->DATA.reg = Frame;

	/* Wait for the frame to actually leave before releasing the pad.  DRE only
	   says the DATA register is free; on this synchronous SLAVE link a frame
	   leaves only when the ECU clocks it, so clearing TXEN on DRE alone can
	   discard a byte that never went out.  TX shares PAD0 with the ADC's
	   output, so it cannot just be left enabled.  Bounded: the ECU owns the
	   clock, and a stalled link must not wedge this ISR. */
	{
		uint32_t Guard = DIAG_TX_DRAIN_GUARD;
		while (!SERCOM_UsartTxComplete(Diag->Hw.Usart) && --Guard);
		SERCOM_UsartTxClearComplete(Diag->Hw.Usart);
		if (Guard == 0)
			Diag_TxDrainTimeouts++;
	}

	SERCOM_UsartTxDisable(Diag->Hw.Usart);
}

#define Diag_TxData(Diag, Byte)     Diag_TxFrame((Diag), (Byte) & 0x00FF)
#define Diag_TxCommand(Diag, Byte)  Diag_TxFrame((Diag), 0x0100 | ((Byte) & 0x00FF))

/* A complete group (announcement plus payload) has arrived.  Decide the single
   byte we send back, if any. */
static void Diag_GroupComplete(Diag_t *Diag)
{
	switch (Diag_Mode)
	{
	case DIAG_MODE_READ_ADDR_MSB:
		/* ECU idle and waiting - start whichever transaction is pending. */
		if (Diag->ReadState == DIAG_READ_READY)
		{
			DIAG_ScopeHigh();
			Diag_TxData(Diag, Diag->ReadAddress >> 8);
		}
#ifdef BUILD_DIAG_WRITE
		else if (Diag->WriteAddressReady || Diag->WriteDataReady)
		{
			DIAG_ScopeHigh();
			Diag_TxCommand(Diag, (Diag->WriteWidth == 1)
			                   ? DIAG_MODE_WRITE8_ADDR_MSB
			                   : DIAG_MODE_WRITE_ADDR_MSB);
		}
#endif
		break;

	case DIAG_MODE_READ_ADDR_LSB:
		Diag_TxData(Diag, Diag->ReadAddress);
		break;

	case DIAG_MODE_READ_RESULT:
		/* Only take this if a read is actually outstanding.  The ECU stays in
		   D8 and streams the NEXT word every poll until a command stops it -
		   that is the block read - so if our stop command is missed (the ECU
		   only listens for it in a ~20us window at the end of the poll) the
		   next word arrives unasked, for an address two higher than the one
		   requested.  Storing it completed the already-finished entry a second
		   time, which is where the stray CAN responses and the wrong values
		   came from: a stale streamed word answering the following request. */
		if (Diag->ReadState == DIAG_READ_READY)
		{
			Diag->ReadData[0] = Diag_Payload[0];
			Diag->ReadData[1] = Diag_Payload[1];
			Diag->ReadState = DIAG_READ_AVAILABLE;
			DIAG_ScopeLow();
			OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_READ);
		}
		else
			Diag_ReadStrays++;

		/* Either way, command it back to idle - that both ends the stream and
		   starts the next transaction. */
		Diag_TxCommand(Diag, DIAG_MODE_READ_ADDR_MSB);
		break;

#ifdef BUILD_DIAG_WRITE
	/* The address phase is the same either width - only the mode codes the ECU
	   announces differ, which is how it knows which data phase to enter. */
	case DIAG_MODE_WRITE_ADDR_MSB:
	case DIAG_MODE_WRITE8_ADDR_MSB:
		Diag_TxData(Diag, Diag->WriteAddress >> 8);
		break;

	case DIAG_MODE_WRITE_ADDR_LSB:
	case DIAG_MODE_WRITE8_ADDR_LSB:
		Diag_TxData(Diag, Diag->WriteAddress);
		break;

	case DIAG_MODE_WRITE_DATA:
	case DIAG_MODE_WRITE8_DATA:
		/* Reaching the data phase means the ECU took both address bytes - that
		   is the address phase acknowledged.  Retire it and wake the task to
		   pack the first data chunk; we answer on the next announcement.
		   Without this the write stalls here with WriteAddressReady still set,
		   which is why every write after the first came back BUSY. */
		if (Diag->WriteAddressReady)
		{
			Diag->WriteAddressReady = 0;
			OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_WRITE);
			break;
		}

		if (!Diag->WriteDataReady)
			/* Nothing more to write - take the ECU back to idle. */
			Diag_TxCommand(Diag, DIAG_MODE_READ_ADDR_MSB);
		else if (Diag_Mode == DIAG_MODE_WRITE8_DATA)
			/* The byte lands the moment the ECU takes it - there is no second
			   half and nothing is held, so an abandoned byte write is simply a
			   byte that was never sent. */
			Diag_TxData(Diag, Diag->WriteData);
		else
			Diag_TxData(Diag, Diag->WriteData >> 8);
		break;

	case DIAG_MODE_WRITE_DATA_LSB:
		/* The ECU is holding our MSB and will not store anything until this
		   arrives - it commits both halves with a single st d,[y].  If we go
		   quiet here it simply re-announces D4 next poll and nothing has been
		   written, so an abandoned write cannot tear a word either. */
		if (Diag->WriteDataReady)
			Diag_TxData(Diag, Diag->WriteData);
		else
			Diag_TxCommand(Diag, DIAG_MODE_READ_ADDR_MSB);
		break;

	case DIAG_MODE_WRITE_ACK:
	case DIAG_MODE_WRITE8_ACK:
		{
			/* Payload is what the ECU read back from where it just wrote, in
			   one instruction either width - so it is a coherent sample of the
			   location, not bytes read a poll apart. */
			const uint16_t Read = (Diag_Mode == DIAG_MODE_WRITE8_ACK)
			                    ? Diag_Payload[0]
			                    : (uint16_t)(((uint16_t)Diag_Payload[0] << 8) | Diag_Payload[1]);

			if (Read == Diag->WriteData)
			{
				Diag->WriteDataReady = 0;
				DIAG_ScopeLow();
				OS_SignalSend(DIAG_TASK_ID, DIAG_SIGNAL_WRITE);

				if (Diag->WriteSize == 0 && Diag_WriteCompleteHandler)
					Diag_WriteCompleteHandler(true);
			}
			/* On a mismatch leave WriteDataReady set, so the value is resent
			   when the ECU next announces its data state.  Never silently
			   accept. */
		}
		break;
#endif

	default:
		Diag_ProtocolResyncs++;
		break;
	}
}

/* Every received frame lands here. */
static void Diag_RxHandler(Diag_t *Diag, uint16_t RxData)
{
	if (RxData & 0x0100)
	{
		/* Mark parity.  D0..DF is the ECU announcing its state; 00..1F is its
		   ADC traffic, which is none of our business. */
		const uint8_t Byte = (uint8_t)RxData;
		if (Byte >= 0xD0)
		{
			Diag_Mode         = Byte;
			Diag_PayloadIndex = 0;
			Diag_PayloadLeft  = (Byte == DIAG_MODE_READ_RESULT ||
			                     Byte == DIAG_MODE_WRITE_ACK)  ? 2 :
			                    (Byte == DIAG_MODE_WRITE8_ACK) ? 1 : 0;
			if (Diag_PayloadLeft == 0)
				Diag_GroupComplete(Diag);
		}
		return;
	}

	/* Space parity: payload for the announcement we are inside.  A payload
	   byte with no announcement is a stray and is dropped rather than taken as
	   data - the v1 defect that turned a timing miss into a wrong value
	   instead of a missing one. */
	if (Diag_PayloadLeft)
	{
		if (Diag_PayloadIndex < sizeof(Diag_Payload))
			Diag_Payload[Diag_PayloadIndex++] = (uint8_t)RxData;

		if (--Diag_PayloadLeft == 0)
			Diag_GroupComplete(Diag);
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
		else
		{
			/* Receive error (overrun/framing).  Abandon the group we were in so
			   a lost frame cannot be absorbed as payload; the ECU's next
			   announcement resynchronises us. */
			Diag_PayloadLeft = 0;
			Diag_ProtocolResyncs++;
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
	/* Highest priority in the system.  The ECU is the master of this link and
	   its ROM waits only 16 poll-loop iterations (ld b,#10h) for our reply
	   before abandoning the transaction with a bare ret.  At priority 1 this
	   handler could not preempt TC0/TC1 (the SDL sniffers, also priority 1),
	   so a 4 ms inter-CPU burst would push us past that window - the ECU
	   would give up while our read/write sequence stayed armed, and the next
	   two frames it sent (ADC traffic, 0x101 then 0x102) got captured as the
	   result.  That is the 0x0102 seen on ~12% of reads at 7000 rpm. */
#if DIAG_MARK
	PIO_Clear(DIAG_MARK_PIN);
	PIO_EnableOutput(DIAG_MARK_PIN);
	PIO_Clear(DIAG_MARK2_PIN);
	PIO_EnableOutput(DIAG_MARK2_PIN);
#endif
#if DIAG_SCOPE_MARK
	PIO_Clear(DIAG_SCOPE_PIN);
	PIO_EnableOutput(DIAG_SCOPE_PIN);
#endif
	NVIC_SetPriority(SERCOM_GetIrqNumber(Diag->Instance), 0);
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
			/* One chunk of exactly WriteWidth bytes.  A word request with an
			   odd byte left over cannot be finished atomically, so it is
			   dropped rather than emitted as a half word - use width 1 for
			   odd lengths. */
			const uint8_t WriteSize = (Diag->WriteSize >= Diag->WriteWidth)
			                        ? Diag->WriteWidth : 0;

			if (WriteSize == 2)
				Diag->WriteData = ((uint16_t)Diag->WriteBuffer[Diag->WriteIndex + 0] << 8)
				                | Diag->WriteBuffer[Diag->WriteIndex + 1];
			else if (WriteSize == 1)
				Diag->WriteData = Diag->WriteBuffer[Diag->WriteIndex];

			Diag->WriteIndex   += WriteSize;
			Diag->WriteAddress += WriteSize;
			Diag->WriteSize    -= WriteSize;

			/* Ready if a chunk was just prepared - NOT if bytes remain after
			   it, which is what this used to test.  For a single write the
			   remainder is zero, so the data phase never started. */
			Diag->WriteDataReady = (WriteSize != 0);
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
