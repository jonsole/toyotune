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
#include "sdl.h"
#include "xmem.h"
#include "dmcu.h"
#include "os.h"
#include "evsys.h"
#include "can.h"
#include "debug.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>


// Inter-CPU DMA, TX direction: what CPU1 sends to CPU2.
// 38 bytes - MR2 at CPU1 RAM 0x200..0x225, ST205 at 0x1FA..0x21F.  Same
// size and same slots in both families, so one struct serves both; the
// address map below is the ST205's.
typedef struct
{
	uint16_t PIM2;  
	uint16_t TPS;
	uint16_t ECT;
	uint16_t INJ_PW;
	uint16_t PIM;
	uint8_t THA;
	uint8_t THAM;
	uint8_t Battery;
	uint8_t Reserved2;
	uint8_t StartupCount;	
	uint8_t Reserved3;
	uint8_t UnknownTrim;
	uint8_t Reserved4;
	uint8_t AdcLambda;
	uint8_t KnockBands[3];
	uint8_t Reserved5;
	uint8_t Obd1Injection;
	uint8_t Obd1Ignition;
	uint8_t ObjIscv;
	uint8_t FuelTrim;			/* NOTE: the two ROMs disagree on this slot -
							   the ST205 calls it fuel trim, the MR2 calls the same
							   offset dmatx_obd_o2_sensor.  One annotation is wrong;
							   confirm before relying on it. */
	uint8_t Knock;				/* MR2 dmatx_knock_retard */
	uint8_t Unknown216;
	uint8_t dmatx_tps_delta;
	uint16_t ErrorFlags;
	uint8_t Flags46;
	uint8_t Flags;
	uint8_t LimiterFlags;
	uint8_t IoFlags[3];	
} ECU_DmaData1_t;


// Inter-CPU DMA, RX direction: what CPU1 receives back from CPU2.
// 34 bytes, copied by copy_dma_rx - ST205 into CPU1 RAM 0x220..0x241,
// MR2 into 0x226..0x247.  Same size and same slots in both, so one struct
// serves both families.  The first five fields are 16-bit because the copy
// loop moves 16 bits at a time from an even address; the rest are bytes.
//
// Names are taken from whichever ROM annotates a slot best; the MR2
// (D151803-9651) is further along than the ST205 (D151804-0461), so most
// come from there.  Slots neither ROM resolves keep the ST205 address in
// the name, matching the convention in the TX struct above.
typedef struct
{
	uint16_t Unknown220;
	uint16_t Unknown222;
	uint16_t Unknown224;
	uint16_t ScaledVe;
	uint16_t RpmX5p12;			/* RPM = value / 5.12; MSB * 50 is a good approximation */
	uint8_t WarmupEnrichment;
	uint8_t FuelTrim;			/* MR2 dmarx_fuel_trim_231 */
	uint8_t Enrich1;			/* MR2 dmarx_enrich_232 */
	uint8_t Enrich2;			/* MR2 dmarx_enrich_233 */
	uint8_t EnrichUnknown22E;
	uint8_t ThamEnrichUnknown;
	uint8_t Unknown230;
	uint8_t FuelEnrich;
	uint8_t Unknown232;
	uint8_t Unknown233;
	uint8_t Unknown234;
	uint8_t Unknown235;
	uint8_t Unknown236;
	uint8_t IgnTiming;
	uint8_t IgnTimingFallback1;
	uint8_t Unknown239;
	uint8_t Unknown23A;
	uint8_t Unknown23B;
	uint8_t Unknown23C;
	uint8_t Unknown23D;
	uint8_t Unknown23E;
	uint8_t Unknown23F;
	uint8_t Unknown240;
	uint8_t Unknown241;
} ECU_DmaData2_t;

/* These two structs are wire layouts: they must match the ECU's DMA blocks
   byte for byte.  Compilers are free to insert padding, which would silently
   shift every field past the padding, so fail the build instead.  (C99 has no
   _Static_assert; a negative array size is the portable equivalent.) */
typedef char ECU_DmaData1_SizeCheck[(sizeof(ECU_DmaData1_t) == TOYOTUNE_DMA_TX_FRAME_SIZE) ? 1 : -1];
typedef char ECU_DmaData2_SizeCheck[(sizeof(ECU_DmaData2_t) == TOYOTUNE_DMA_RX_FRAME_SIZE) ? 1 : -1];

/* The Denso CPU is big-endian (D = A:B with A the high byte, M68HC11 style)
   while the SAMC21 is little-endian, so every 16-bit field in the two structs
   above arrives byte swapped.  Pass one through this before using it as a
   number - e.g. RPM from Unknown228 would be ECU_Be16(Rx.Unknown228) / 5.12.

   NOTE the existing 0x1001 telemetry frame copies ECT/PIM/TPS straight
   through without swapping, so those 16-bit values are byte swapped on the
   bus as things stand.  Left alone deliberately rather than silently changing
   what any existing tooling already decodes. */
static __inline uint16_t ECU_Be16(uint16_t Value)
{
	return (uint16_t)((Value >> 8) | (Value << 8));
}

typedef struct  
{
	uint16_t PIM;
	uint16_t TPS;
	uint16_t ECT;
	uint8_t THA;
	uint8_t THAM;
} ECU_CanFrame1001_t;

typedef struct
{
	uint8_t KnockBands[3];
	uint8_t Knock;
} ECU_CanFrame1002_t;

/*
RAM:01FA ?? ??       dmatx_pim2:                     .block 2                        ; DATA XREF: ROM:C5FAt sub_C57A+1FFt start_dma+27t ROM:FBB1w
RAM:01FC ?? ??       dmatx_tps:                      .block 2                        ; DATA XREF: ROM:FC77w ROM:FCF6w
RAM:01FE ?? ??       dmatx_ect:                      .block 2                        ; DATA XREF: ROM:FE4Dw
RAM:0200 ?? ??       dmatx_inj_pw:                   .block 2                        ; DATA XREF: dma_tx_copy+26w
RAM:0202 ?? ??       dmatx_pim:                      .block 2                        ; DATA XREF: sub_C57A+8EAr sub_C57A:loc_E59Bw sub_C57A+206Fr sub_E7D9:loc_E7E0r
RAM:0204 ??          dmatx_tha:                      .block 1                        ; DATA XREF: ROM:FDB6w
RAM:0205 ??          dmatx_tham:                     .block 1                        ; DATA XREF: ROM:FDEDw
RAM:0206 ??          dmatx_battery:                  .block 1                        ; DATA XREF: ROM:FDF5w
RAM:0207 ??          byte_207:                       .block 1                        ; DATA XREF: dma_tx_copy:loc_F8A3w
RAM:0208 ??          dmatx_cnt_startup:              .block 1                        ; DATA XREF: dma_tx_copy+16w
RAM:0209 ??          byte_209:                       .block 1                        ; DATA XREF: dma_tx_copy+1Bw
RAM:020A ??          dmatx_unk_trim:                 .block 1                        ; DATA XREF: dma_tx_copy:loc_F8ADw
RAM:020B ??          byte_20B:                       .block 1                        ; DATA XREF: dma_tx_copy+20w
RAM:020C ??          dmatx_adc_lambda:               .block 1                        ; DATA XREF: ROM:FC13w
RAM:020D ?? ?? ??    dmatx_knock_info:               .block 3                        ; DATA XREF: dma_tx_copy+2Cw dma_tx_copy+32w
RAM:0210 ??          byte_210:                       .block 1                        ; DATA XREF: sub_E996+49Fr ROM:F510r ROM:loc_F5BBw ROM:loc_F5D0r
RAM:0211 ??          dmatx_obd1_inj:                 .block 1                        ; DATA XREF: ROM:DD2Ew
RAM:0212 ??          dmatx_obd_ign:                  .block 1                        ; DATA XREF: iv6_ne_process+99w iv6_ne_process+13Cw
RAM:0213 ??          dmatx_obd_iscv:                 .block 1                        ; DATA XREF: ROM:DD37w
RAM:0214 ??          dmatx_fuel_trim:                .block 1                        ; DATA XREF: sub_C57A:loc_D12Bw
RAM:0215 ??          dmatx_knock:                    .block 1                        ; DATA XREF: sub_E996:loc_EE50w iv6_ne_process+F9r
RAM:0216 ??          dmatx_unk_216:                  .block 1                        ; DATA XREF: dma_tx_copy+38w
RAM:0217 ??          dmatx_tps_delta:                .block 1                        ; DATA XREF: dma_tx_copy+3Dw
RAM:0218 ?? ??       dmatx_error_flags:              .block 2                        ; DATA XREF: dma_tx_copy+42w
RAM:021A ??          dmatx_flags_46:                 .block 1                        ; DATA XREF: dma_tx_copy+47w
RAM:021B ??          dmatx_flags_1:                  .block 1                        ; DATA XREF: dma_tx_copy:loc_F911w
RAM:021C ??          dmatx_limiter_flags:            .block 1                        ; DATA XREF: dma_tx_copy+7Aw
RAM:021D ??          dmatx_io_flags_1:               .block 1                        ; DATA XREF: sub_E094+35w sub_E094:loc_E0E7w
RAM:021E ?? ??       dmatx_io_flags_2:               .block 2                        ; DATA XREF: sub_E094+3Cw sub_E094:loc_E0F8w sub_E094+85w sub_E094+1A0w
*/





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
	ECU_DmaData1_t Tx;			/* CPU1 -> CPU2, 38 bytes */
	ECU_DmaData2_t Rx;			/* CPU2 -> CPU1, 34 bytes */
	volatile bool TxValid;
	volatile bool RxValid;
	volatile uint32_t TxCount;	/* whole frames captured, for link health */
	volatile uint32_t RxCount;
} ECU_Dma;


bool ECU_GetDmaSnapshot(ECU_DmaData1_t *Tx, ECU_DmaData2_t *Rx)
{
	bool Valid;

	OS_InterruptDisable();
	Valid = ECU_Dma.TxValid && ECU_Dma.RxValid;
	if (Tx)
		*Tx = ECU_Dma.Tx;
	if (Rx)
		*Rx = ECU_Dma.Rx;
	OS_InterruptEnable();

	return Valid;
}


/* CPU2 -> CPU1 direction, SERCOM1 / DMA channel 0. */
void ECU_DmaData2(SDL_t *Sdl, void *Data, const uint8_t *RxBuffer, uint8_t RxSize)
{
	/* Only a whole frame is usable; a partial capture would misalign every
	   field after the truncation. */
	if (RxSize != TOYOTUNE_DMA_RX_FRAME_SIZE)
		return;

	ECU_Dma.Rx = *(const ECU_DmaData2_t *)RxBuffer;
	ECU_Dma.RxValid = true;
	ECU_Dma.RxCount += 1;
}


void ECU_DmaData1(SDL_t *Sdl, void *Data, const uint8_t *RxBuffer, uint8_t RxSize)
{
	ECU_DmaData1_t *EcuData = (ECU_DmaData1_t *)RxBuffer;
	if (RxSize == TOYOTUNE_DMA_TX_FRAME_SIZE)
	{
		ECU_Dma.Tx = *EcuData;
		ECU_Dma.TxValid = true;
		ECU_Dma.TxCount += 1;

		ECU_CanFrame1001_t Frame1001;
		Frame1001.ECT = EcuData->ECT;
		Frame1001.PIM = EcuData->PIM;
		Frame1001.THA = EcuData->THA;
		Frame1001.THAM = EcuData->THAM;
		Frame1001.TPS = EcuData->TPS;
		CAN_Tx(TOYOTUNE_CAN_ID_TELEMETRY_1, &Frame1001, sizeof(Frame1001));

		ECU_CanFrame1002_t Frame1002;
		memcpy(Frame1002.KnockBands, EcuData->KnockBands, 3);
		Frame1002.Knock = EcuData->Knock;
		CAN_Tx(TOYOTUNE_CAN_ID_TELEMETRY_2, &Frame1002, sizeof(Frame1002));		
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

	/* Initialise serial data logger, DMA Channel 0, SERCOM1 with Rx on pad 2 */
	/* D151804-0461 RX - 34 bytes, the CPU2 -> CPU1 direction */
	SDL_Init(&Sdl[0], 0, 1, 2, 64, ECU_DmaData2, NULL);

	/* Initialise serial data logger, DMA Channel 1, SERCOM2 with Rx on pad 0 */
	/* D151804-0471 TX - 38 bytes */
	SDL_Init(&Sdl[1], 1, 2, 0, 64, ECU_DmaData1, NULL);

	/* Initialise diagnostics, SERCOM0 */
	Diag_Init();

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
	//OS_SignalSend(ESP_TASK_ID, ESP_SIGNAL_RX_IDLE);
	//Diag_TimerTick(&Diag);
}

