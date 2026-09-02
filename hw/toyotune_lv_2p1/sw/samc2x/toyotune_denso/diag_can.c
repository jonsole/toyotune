/*
 * diag_can.c
 *
 * Reading and writing the running Denso MCU's memory from the CAN bus.
 *
 * diag.c owns the serial protocol to the ECU; this maps it onto two CAN
 * frames, TOYOTUNE_CAN_ID_DIAG_CMD in and TOYOTUNE_CAN_ID_DIAG_RSP out.
 *
 * Command frame (8 bytes, big-endian to match the ECU's own byte order):
 *
 *   0    opcode
 *   1-2  address
 *   3-4  value        writes only
 *   5-6  period ms    add-periodic only
 *   7    size, 1 or 2 reads and add-periodic
 *
 * Response frame:
 *
 *   0    opcode echoed
 *   1    status, 0 = ok
 *   2-3  address echoed, so a host can correlate replies
 *   4-5  value
 *   6-7  zero
 *
 * A periodic read emits the same response frame on its own schedule, so a
 * host decodes one layout for both.
 *
 * Entries come from a fixed pool rather than the heap: the count is bounded,
 * allocation cannot fail unpredictably, and there is no free to get wrong in
 * an interrupt path.
 */

#include <sam.h>
#include <string.h>

#include "config.h"
#include "can.h"
#include "diag.h"
#include "diag_can.h"
#include "os.h"

#define DIAG_CAN_OP_READ			(0x01)
#define DIAG_CAN_OP_WRITE			(0x02)
#define DIAG_CAN_OP_ADD_PERIODIC	(0x03)
#define DIAG_CAN_OP_CANCEL_PERIODIC	(0x04)
#define DIAG_CAN_OP_CANCEL_ALL		(0x05)

#define DIAG_CAN_OK					(0x00)
#define DIAG_CAN_ERR_OPCODE			(0x01)
#define DIAG_CAN_ERR_NO_SPACE		(0x02)
#define DIAG_CAN_ERR_NOT_FOUND		(0x03)
#define DIAG_CAN_ERR_SIZE			(0x04)
#define DIAG_CAN_ERR_BUSY			(0x05)

/* Bounded, and generous: the ECU link is the bottleneck long before this is. */
#define DIAG_CAN_ENTRIES			(8)

static Diag_ReadEntry_t DiagCan_Entries[DIAG_CAN_ENTRIES];
static bool DiagCan_EntryUsed[DIAG_CAN_ENTRIES];

static uint16_t DiagCan_WriteAddress;
static uint16_t DiagCan_WriteValue;

uint32_t DiagCan_Commands;
uint32_t DiagCan_Rejected;


static uint16_t DiagCan_Be16(const uint8_t *Data)
{
	return (uint16_t)((Data[0] << 8) | Data[1]);
}


static void DiagCan_Respond(uint8_t Opcode, uint8_t Status, uint16_t Address, uint16_t Value)
{
	uint8_t Payload[8];

	Payload[0] = Opcode;
	Payload[1] = Status;
	Payload[2] = (uint8_t)(Address >> 8);
	Payload[3] = (uint8_t)Address;
	Payload[4] = (uint8_t)(Value >> 8);
	Payload[5] = (uint8_t)Value;
	Payload[6] = 0;
	Payload[7] = 0;

	CAN_TxStandard(TOYOTUNE_CAN_ID_DIAG_RSP, Payload, sizeof(Payload));
}


static Diag_ReadEntry_t *DiagCan_EntryAlloc(void)
{
	for (uint8_t i = 0; i < DIAG_CAN_ENTRIES; i++)
	{
		if (!DiagCan_EntryUsed[i])
		{
			DiagCan_EntryUsed[i] = true;
			memset(&DiagCan_Entries[i], 0, sizeof(DiagCan_Entries[i]));
			return &DiagCan_Entries[i];
		}
	}

	return NULL;
}


static void DiagCan_EntryFree(Diag_ReadEntry_t *Entry)
{
	const uint32_t Index = (uint32_t)(Entry - DiagCan_Entries);

	if (Index < DIAG_CAN_ENTRIES)
		DiagCan_EntryUsed[Index] = false;
}


/* Unlink a queued entry by address.  Walks diag.c's list directly, which is
   why this runs with interrupts disabled - the receive interrupt can take the
   head of the same list. */
static bool DiagCan_Cancel(uint16_t Address, bool All)
{
	bool Removed = false;

	OS_InterruptDisable();

	Diag_ReadEntry_t **Link = &Diag.ReadList;
	while (*Link)
	{
		Diag_ReadEntry_t *Entry = *Link;

		if (All || Entry->Address == Address)
		{
			*Link = Entry->Next;
			DiagCan_EntryFree(Entry);
			Removed = true;
		}
		else
		{
			Link = &Entry->Next;
		}
	}

	/* An entry that is mid-transfer has already been taken off the list into
	   ReadCurrent, so the walk above cannot see it and completion would
	   re-queue it.  Marking it one-shot makes it report once and stop. */
	if (Diag.ReadCurrent && Diag.ReadCurrent->Period != 0 &&
	    (All || Diag.ReadCurrent->Address == Address))
	{
		Diag.ReadCurrent->Period = 0;
		Removed = true;
	}

	OS_InterruptEnable();

	return Removed;
}


/* A completed read, one-shot or periodic, reports the same way. */
/* The task advances Diag.WriteAddress while preparing the data phase, so
   report the address that was actually asked for rather than reading it
   back out of the diag state. */
static void DiagCan_WriteComplete(bool Ok)
{
	DiagCan_Respond(DIAG_CAN_OP_WRITE, Ok ? DIAG_CAN_OK : DIAG_CAN_ERR_BUSY,
	                DiagCan_WriteAddress, DiagCan_WriteValue);
}


static void DiagCan_ReadComplete(Diag_ReadEntry_t *Entry, uint16_t Value)
{
	const bool OneShot = (Entry->Period == 0);

	DiagCan_Respond(OneShot ? DIAG_CAN_OP_READ : DIAG_CAN_OP_ADD_PERIODIC,
	                DIAG_CAN_OK, Entry->Address, Value);

	if (OneShot)
		DiagCan_EntryFree(Entry);
}


static void DiagCan_Queue(uint8_t Opcode, uint16_t Address, uint8_t Size, uint16_t Period)
{
	Diag_ReadEntry_t *Entry = DiagCan_EntryAlloc();
	if (!Entry)
	{
		DiagCan_Rejected += 1;
		DiagCan_Respond(Opcode, DIAG_CAN_ERR_NO_SPACE, Address, 0);
		return;
	}

	Entry->Address = Address;
	Entry->Size = Size;
	Entry->Period = Period;
	Entry->Time = Diag_Time() + Period;

	OS_InterruptDisable();
	Diag_ReadEntryInsert(&Diag, Entry);
	OS_InterruptEnable();
}


/* Runs in CAN interrupt context.  Deliberately does only list and register
   work - the actual ECU transfer is driven by diag.c's own state machine off
   the ECU's traffic, so there is nothing to block on here. */
static void DiagCan_Command(uint16_t Id, const uint8_t *Data, uint8_t Length)
{
	if (Id != TOYOTUNE_CAN_ID_DIAG_CMD || Length < 3)
		return;

	DiagCan_Commands += 1;

	const uint8_t Opcode = Data[0];
	const uint16_t Address = DiagCan_Be16(&Data[1]);
	const uint16_t Value = (Length >= 5) ? DiagCan_Be16(&Data[3]) : 0;
	const uint16_t Period = (Length >= 7) ? DiagCan_Be16(&Data[5]) : 0;
	const uint8_t Size = (Length >= 8 && Data[7] != 0) ? Data[7] : 2;

	if (Size != 1 && Size != 2)
	{
		DiagCan_Respond(Opcode, DIAG_CAN_ERR_SIZE, Address, 0);
		return;
	}

	switch (Opcode)
	{
		case DIAG_CAN_OP_READ:
			/* Period 0 marks it one-shot; it is not re-queued on completion */
			DiagCan_Queue(Opcode, Address, Size, 0);
			break;

		case DIAG_CAN_OP_ADD_PERIODIC:
			if (Period == 0)
				DiagCan_Respond(Opcode, DIAG_CAN_ERR_SIZE, Address, 0);
			else
				DiagCan_Queue(Opcode, Address, Size, Period);
			break;

		case DIAG_CAN_OP_CANCEL_PERIODIC:
			DiagCan_Respond(Opcode,
			                DiagCan_Cancel(Address, false) ? DIAG_CAN_OK : DIAG_CAN_ERR_NOT_FOUND,
			                Address, 0);
			break;

		case DIAG_CAN_OP_CANCEL_ALL:
			DiagCan_Cancel(0, true);
			DiagCan_Respond(Opcode, DIAG_CAN_OK, 0, 0);
			break;

		case DIAG_CAN_OP_WRITE:
			if (Diag.WriteAddressReady || Diag.WriteDataReady)
			{
				DiagCan_Respond(Opcode, DIAG_CAN_ERR_BUSY, Address, Value);
				break;
			}

			Diag.WriteBuffer[0] = (uint8_t)(Value >> 8);
			Diag.WriteBuffer[1] = (uint8_t)Value;
			Diag.WriteIndex = 0;
			Diag.WriteSize = Size;
			Diag.WriteAddress = Address;
			Diag.WriteAddressAck = 0;
			Diag.WriteAddressCommand = (Size > 1) ? 0xDB : 0xDD;
			Diag.WriteAddressReady = true;

			/* No response yet - DiagCan_WriteComplete sends it once the ECU has
			   acknowledged the data, so an OK means the write actually landed. */
			DiagCan_WriteAddress = Address;
			DiagCan_WriteValue = Value;
			break;

		default:
			DiagCan_Rejected += 1;
			DiagCan_Respond(Opcode, DIAG_CAN_ERR_OPCODE, Address, 0);
			break;
	}
}


void DiagCan_Init(void)
{
	Diag_SetReadCompleteHandler(DiagCan_ReadComplete);
	Diag_SetWriteCompleteHandler(DiagCan_WriteComplete);
	CAN_RxSetHandler(DiagCan_Command);
}
