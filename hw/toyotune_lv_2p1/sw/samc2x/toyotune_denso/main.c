/*
 * toyotune_denso.c
 *
 * Created: 27/05/2016 20:20:53
 * Author : Jon
 */ 

#include <sam.h>

#include "config.h"
#include "mem.h"
#include "dmac.h"
#include "clk.h"
#include "diag.h"
#include "diag_can.h"
#include "sdl.h"
#include "xmem.h"
#include "dmcu.h"
#include "os.h"
#include "evsys.h"
#include "can.h"
#include "can_telemetry.h"
#include "debug.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>


#include "ecu.h"







SDL_t Sdl[2];

volatile int count1 = 0;
volatile int count2 = 0;

volatile int Timer = 0;

uint32_t TestTask1Stack[50];
void TestTask1(void)
{
	while (1)
	{
		OS_Message_t *MyMsg;
		OS_Message_t *RcvMsg;

		MyMsg = MEM_Create(OS_Message_t);
		OS_MessageSend(MyMsg, 2);
		RcvMsg = OS_MessageWait();
		MEM_Free(RcvMsg);
	}
}

uint32_t TestTask2Stack[50];
void TestTask2(void)
{
	while (1)
	{
		OS_Message_t *MyMsg;
		OS_Message_t *RcvMsg;

		RcvMsg = OS_MessageWait();
		MEM_Free(RcvMsg);
		MyMsg = MEM_Create(OS_Message_t);
		OS_MessageSend(MyMsg, 1);
	}
}

/* Latest complete copy of each direction of the inter-CPU link.  Written by
   the sdl.c callbacks, which run in TC interrupt context, so anything reading
   these from a task must take a consistent copy with interrupts masked -
   ECU_GetDmaSnapshot() does that.  Valid flags stay clear until a first whole
   frame has been seen, so a consumer can tell 'no data yet' from 'all zero'. */
static struct
{
	ECU_DmaData1_t Cpu1ToCpu2;		/* 38 bytes */
	ECU_DmaData2_t Cpu2ToCpu1;		/* 34 bytes */
	volatile bool Cpu1ToCpu2Valid;
	volatile bool Cpu2ToCpu1Valid;
	volatile uint32_t Cpu1ToCpu2Count;	/* whole frames captured, for link health */
	volatile uint32_t Cpu2ToCpu1Count;
} ECU_Dma;


bool ECU_GetDmaSnapshot(ECU_DmaData1_t *Cpu1ToCpu2, ECU_DmaData2_t *Cpu2ToCpu1)
{
	bool Valid;

	OS_InterruptDisable();
	Valid = ECU_Dma.Cpu1ToCpu2Valid && ECU_Dma.Cpu2ToCpu1Valid;
	if (Cpu1ToCpu2)
		*Cpu1ToCpu2 = ECU_Dma.Cpu1ToCpu2;
	if (Cpu2ToCpu1)
		*Cpu2ToCpu1 = ECU_Dma.Cpu2ToCpu1;
	OS_InterruptEnable();

	return Valid;
}


/* The CPU2 -> CPU1 block, whichever SERCOM happens to carry it. */
void ECU_DmaCpu2ToCpu1(SDL_t *Sdl, void *Data, const uint8_t *RxBuffer, uint8_t RxSize)
{
	/* Only a whole frame is usable; a partial capture would misalign every
	   field after the truncation. */
	if (RxSize != TOYOTUNE_DMA_CPU2_TO_CPU1_SIZE)
		return;

	const ECU_DmaData2_t *EcuData = (const ECU_DmaData2_t *)RxBuffer;

	ECU_Dma.Cpu2ToCpu1 = *EcuData;
	ECU_Dma.Cpu2ToCpu1Valid = true;
	ECU_Dma.Cpu2ToCpu1Count += 1;

}


/* The CPU1 -> CPU2 block, whichever SERCOM happens to carry it. */
void ECU_DmaCpu1ToCpu2(SDL_t *Sdl, void *Data, const uint8_t *RxBuffer, uint8_t RxSize)
{
	ECU_DmaData1_t *EcuData = (ECU_DmaData1_t *)RxBuffer;
	if (RxSize == TOYOTUNE_DMA_CPU1_TO_CPU2_SIZE)
	{
		ECU_Dma.Cpu1ToCpu2 = *EcuData;
		ECU_Dma.Cpu1ToCpu2Valid = true;
		ECU_Dma.Cpu1ToCpu2Count += 1;

	}
	//Debug("%d %u %02x %02x\n", Timer, RxSize, RxBuffer[12], RxBuffer[14]);
}


int main(void)
{
	CLK_Init();

    /* Initialize the SAM system */
    SystemInit();

	/* Initialise memory pools */
	MEM_Init();

	/* Initialise OS */
	OS_Init();

	/* Initialise DMA controller */
	DMAC_Init();

	/* Initialise event system */
	EVSYS_Init();

	/* Initialise CAN controller */
	CAN_Init();
	
	/* Initialise debug output on SERCOM3 */
	//Debug_Init(3);
	//Debug("Toyotune SAMC21 v0.2, Copyright 2022, Jon Sole\n");

	/* Initialise Denso MCU and external memory */
	DMCU_Init();
	XMEM_Init();

	/* Read CPLD version */
	uint8_t CpldVersion;
	XMEM_BlockRead(&CpldVersion, DMCU_REG_VER, sizeof(CpldVersion));
	Debug("CPLD Version %d\n", CpldVersion);
	PanicFalse(CpldVersion == 4);
	
	/* Copy image to SRAM */
	XMEM_WriteEnable(1);	
	XMEM_BlockWrite(0x8000, DMCU_Image, 32768);
	XMEM_WriteEnable(0);

	/* Release reset on Denso MCU */
	DMCU_ResetDisable();

	/* Both serial data loggers sniff the inter-CPU link: DMA channel 0 on
	   SERCOM1 with Rx on pad 2, DMA channel 1 on SERCOM2 with Rx on pad 0.

	   Which SERCOM carries which block depends on the CPU this board is
	   plugged into, since it taps that CPU's own link pins.  The blocks
	   themselves are identical either way - only the roles swap - so one
	   binary serves both boards with just this binding reversed. */
#if defined(TOYOTUNE_CPU1)
	SDL_Init(&Sdl[0], 0, 1, 2, 64, ECU_DmaCpu2ToCpu1, NULL);	/* CPU1 receives */
	SDL_Init(&Sdl[1], 1, 2, 0, 64, ECU_DmaCpu1ToCpu2, NULL);	/* CPU1 transmits */
#else
	SDL_Init(&Sdl[0], 0, 1, 2, 64, ECU_DmaCpu1ToCpu2, NULL);	/* CPU2 receives */
	SDL_Init(&Sdl[1], 1, 2, 0, 64, ECU_DmaCpu2ToCpu1, NULL);	/* CPU2 transmits */
#endif

	/* Initialise diagnostics, SERCOM0 */
	Diag_Init();
	DiagCan_Init();

	/* Start periodic CAN telemetry */
	CanTelemetry_Init();

	/* Enable SYSTICK 1ms timer */
	SysTick_Config(48000000 / 1000);
	NVIC_EnableIRQ(SysTick_IRQn);

	/* Start OS */
	OS_Start();
}

#include "os_task_id.h"

void SysTick_Handler(void)
{
	Timer += 1;
	CanTelemetry_TimerTick();
	//OS_SignalSend(ESP_TASK_ID, ESP_SIGNAL_RX_IDLE);
	Diag_TimerTick(&Diag);
}

