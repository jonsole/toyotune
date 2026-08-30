#ifndef __DIAG_H_
#define __DIAG_H_

#include <sam.h>
#include <stdbool.h>
#include <time.h>

typedef struct Diag_Hardware
{
	SercomUsart *Usart;
} Diag_Hardware_t;

typedef struct Diag_ReadEntry
{
	struct Diag_ReadEntry *Next;
	time_t Time;
	uint16_t Period;
	uint16_t Address;
	uint8_t Size;
	uint8_t Repeat;
	uint8_t Buffer[2];
} Diag_ReadEntry_t;


typedef struct Diag
{
	Diag_Hardware_t Hw;
	uint8_t Instance;

	void (*RxHandler)(struct Diag *Diag, uint16_t RxData);

	uint8_t AdcChannel;
	uint16_t AdcData[16];

	uint8_t WriteDataCommand;
	uint8_t WriteAddressCommand;

	bool WriteAddressReady;
	bool WriteDataReady;
	//bool WriteDataUpdate;

	uint8_t	WriteSize;
	uint16_t WriteAddress;
	uint16_t WriteAddressAck;	// Ack from ECU of address to write to
	uint8_t	WriteIndex;
	uint8_t	WriteBuffer[32];
	uint16_t WriteData;			//Prepare data to write
	uint16_t WriteDataAck;		// Ack from ECU of data written

	Diag_ReadEntry_t *ReadCurrent;
	uint8_t	ReadState;
	uint8_t ReadData[2];
	
	uint8_t	ReadSize;
	uint16_t ReadAddress;
	uint8_t	ReadIndex;
	uint8_t  ReadRepeat;

	Diag_ReadEntry_t *ReadList;
	
	uint32_t Stack[1024];

} Diag_t;

/* The one and only instance, defined in diag.c. Declared here so other
   translation units (main.c's SysTick handler, for one) can reach it
   without making a second tentative definition of their own. */
extern Diag_t Diag;


static __inline bool DIAG_IsCommand(const uint16_t RxData)
{
	return !(RxData & 0x100);
}

extern void Diag_Init(void);
extern void Diag_Task(void *Context);
extern void Diag_TimerTick(Diag_t *Diag);

#endif
