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
	uint8_t TxInProgress;

	DebugBuffer_t Buffer;

	uint8_t RxUsartDmaChannel;
	uint8_t TxUsartDmaChannel;
	uint8_t TxUsartDmaTrigger;
	uint8_t RxUsartDmaTrigger;

	uint8_t Level;
} Debug_t;

extern Debug_t DebugState;

#if 0

extern void Debug_Init(uint8_t Instance);
extern void Debug_SetLevel(uint8_t Level);
extern uint8_t Debug_GetLevel(void);
extern void Debug_PutChar(char Char, void *Context);
extern void Debug_PrintF(const char *Format, ...);

#ifndef Debug
#define Debug(...)			Debug_PrintF(__VA_ARGS__)
#endif
#define Debug_Test(...)		((DebugState.Level >= 2) ? Debug_PrintF(__VA_ARGS__) : 0)
#define Debug_Info(...)		((DebugState.Level >= 1) ? Debug_PrintF(__VA_ARGS__) : 0)
#define Debug_Error(...)	((DebugState.Level >= 0) ? Debug_PrintF(__VA_ARGS__) : 0)

extern void PanicDebug(const char *FileName, int Line);
#define Panic()			PanicDebug(__FILE__, __LINE__)
#define PanicFalse(e)	do { if (!(e)) PanicDebug(__FILE__, __LINE__); } while(0)
#define PanicNull(e)	do { if ((e) == NULL) PanicDebug(__FILE__, __LINE__); } while(0)

#else

#define Debug_Init(...)
#define Debug_SetLevel(...)
#define Debug_GetLevel()
#define Debug_PutChar(...)
#define Debug_PrintF(...)

#define Debug(...)
#define Debug_Test(...)
#define Debug_Info(...)
#define Debug_Error(...)

#define Panic()
#define PanicFalse(...)
#define PanicNull(...)

#endif


#endif
