#ifndef __DEBUG_H_
#define __DEBUG_H_

#include <stdio.h>
#include <stdint.h>
#include <sam.h>

#define DEBUG_BUFFER_SIZE (2048)

typedef struct
{
	volatile uint16_t Index;
	volatile uint16_t Outdex;
	uint8_t Buffer[DEBUG_BUFFER_SIZE];
} DebugBuffer_t;

typedef struct
{
	SercomUsart *Usart;
	uint8_t Instance;

	DebugBuffer_t Buffer;

	uint8_t RxUsartDmaChannel;
	uint8_t TxUsartDmaChannel;
	uint8_t TxUsartDmaTrigger;
	uint8_t RxUsartDmaTrigger;

	uint8_t Level;
} Debug_t;

extern Debug_t DebugState;

extern void Debug_Init(void);
extern void Debug_SetLevel(uint8_t Level);
extern uint8_t Debug_GetLevel(void);
extern void Debug_PutChar(char Char, void *Context);
extern void Debug_Flush(void);
extern uint8_t Debug_GetChar(void);
extern void Debug_PrintF(const char *Format, ...);

#ifndef Debug
#define Debug(...)			Debug_PrintF(__VA_ARGS__)
#endif
#define Debug_Test(...)		((DebugState.Level >= 2) ? Debug_PrintF(__VA_ARGS__) : 0)
#define Debug_Info(...)		((DebugState.Level >= 1) ? Debug_PrintF(__VA_ARGS__) : 0)
#define Debug_Error(...)	((DebugState.Level >= 0) ? Debug_PrintF(__VA_ARGS__) : 0)

#define Panic()			for(;;)
#define PanicFalse(e)	do { if (!(e)) Panic(); } while(0)
#define PanicNull(e)	do { if ((e) == NULL) Panic(); } while(0)

#endif
