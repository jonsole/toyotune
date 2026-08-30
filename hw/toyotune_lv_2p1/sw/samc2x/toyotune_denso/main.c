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


// Inter-CPU DMA, the CPU1 -> CPU2 block: 38 bytes.
//
// Layout and names taken from the MR2 CPU1 disassembly,
// roms/3S-GTE/D151803-9651/Claude/D151803-9651.asm, which is the ECU this
// board is developed against (CPU1 D151803-9651, CPU2 D151803-9661, board
// plugged into CPU1).  There the block sits at CPU1 RAM 0x200..0x225.
//
// The ST205 pair (D151804-0461/-0471) uses the same geometry and the same
// field slots at a different base (0x1FA..0x21F), so this struct serves both
// families - see TOYOTUNE_ECU_MR2 / TOYOTUNE_ECU_ST205 in config.h.
typedef struct
{
	uint16_t Pim2;				/* 0x200 */
	uint16_t Tps;				/* 0x202 */
	uint16_t Ect;				/* 0x204 */
	uint16_t InjPwInj1;			/* 0x206  injector 1 pulse width, 4us per count */
	uint16_t Pim;				/* 0x208  transient-compensated, fuel from this */
	uint8_t Tha;				/* 0x20A */
	uint8_t Tham;				/* 0x20B */
	uint8_t Battery;			/* 0x20C  raw ADC; volts = raw * 0.0775 */
	uint8_t NvTrimPim;			/* 0x20D */
	uint8_t CmdStartup;			/* 0x20E */
	uint8_t CntUnknown20F;		/* 0x20F */
	uint8_t NvTrimO2;			/* 0x210 */
	uint8_t LambdaState;		/* 0x211 */
	uint8_t AdcLambda;			/* 0x212 */
	uint8_t KnockRetardInfo[3];	/* 0x213  per-cylinder, ~0.5 deg per count */
	uint8_t IgnCorrCpu2;		/* 0x216 */
	uint8_t ObdInj;				/* 0x217 */
	uint8_t IgnObd;				/* 0x218 */
	uint8_t ObdIscv;			/* 0x219 */
	uint8_t ObdO2Sensor;		/* 0x21A  the ST205 disassembly used to call this
							   fuel trim; that annotation was wrong */
	uint8_t KnockRetard;		/* 0x21B  current retard, decays 2 per 4ms */
	uint8_t PwLoopMode;			/* 0x21C  0 open loop, 0xC8 closed loop */
	uint8_t TpsDelta;			/* 0x21D */
	uint8_t ErrorFlags1;		/* 0x21E */
	uint8_t ErrorFlags2;		/* 0x21F */
	uint8_t Flags46;			/* 0x220 */
	uint8_t Flags1;				/* 0x221 */
	uint8_t LimiterFlags;		/* 0x222 */
	uint8_t Unknown223;			/* 0x223 */
	uint16_t Word224;			/* 0x224 */
} ECU_DmaData1_t;


// Inter-CPU DMA, the CPU2 -> CPU1 block: 34 bytes.  MR2 CPU1 RAM
// 0x226..0x247 (ST205 0x220..0x241), copied by copy_dma_rx, which moves 16
// bits at a time until the destination reaches the end of the block - hence
// the first five fields being 16-bit.
typedef struct
{
	uint16_t Word226;			/* 0x226 */
	uint16_t Word228;			/* 0x228 */
	uint16_t Word22A;			/* 0x22A */
	uint16_t ScaledVe;			/* 0x22C */
	uint16_t RpmX5p12;			/* 0x22E  RPM = value / 5.12; MSB * 50 approximates it */
	uint8_t WarmupEnrich;		/* 0x230 */
	uint8_t FuelTrim;			/* 0x231 */
	uint8_t Enrich232;			/* 0x232 */
	uint8_t Enrich233;			/* 0x233 */
	uint8_t EnrichUnknown234;	/* 0x234 */
	uint8_t ThamEnrichUnknown;	/* 0x235 */
	uint8_t IdleEnrich;			/* 0x236 */
	uint8_t FuelEnrich;			/* 0x237 */
	uint8_t Unknown238;			/* 0x238  unnamed in the disassembly too */
	uint8_t FuelIgnCorr;		/* 0x239 */
	uint8_t KnockRetardCpu2;	/* 0x23A */
	uint8_t MaxRetard;			/* 0x23B  = CPU2's dmatx_max_retard_161 */
	uint8_t LambdaTrim;			/* 0x23C */
	uint8_t IgnTiming;			/* 0x23D */
	uint8_t IgnTimingFallback1;	/* 0x23E */
	uint8_t IgnTimingFallback2;	/* 0x23F */
	uint8_t IgnTimingUnknown;	/* 0x240 */
	uint8_t Unknown241;			/* 0x241 */
	uint8_t IscvDuty;			/* 0x242 */
	uint8_t Status1;			/* 0x243 */
	uint8_t Unknown244;			/* 0x244 */
	uint8_t Status2;			/* 0x245 */
	uint8_t IgnAdvanceHi;		/* 0x246 */
	uint8_t IgnAdvanceLo;		/* 0x247 */
} ECU_DmaData2_t;

/* These two structs are wire layouts: they must match the ECU's DMA blocks
   byte for byte.  Compilers are free to insert padding, which would silently
   shift every field past the padding, so fail the build instead.  (C99 has no
   _Static_assert; a negative array size is the portable equivalent.) */
typedef char ECU_DmaData1_SizeCheck[(sizeof(ECU_DmaData1_t) == TOYOTUNE_DMA_CPU1_TO_CPU2_SIZE) ? 1 : -1];
typedef char ECU_DmaData2_SizeCheck[(sizeof(ECU_DmaData2_t) == TOYOTUNE_DMA_CPU2_TO_CPU1_SIZE) ? 1 : -1];

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
Both blocks as declared in the MR2 CPU1 disassembly,
roms/3S-GTE/D151803-9651/Claude/D151803-9651.asm.  The structs above mirror
this exactly.  TX is 0x200..0x225 (38 bytes), RX is 0x226..0x247 (34 bytes);
copy_dma_rx copies the RX block until the pointer reaches byte_248.

RAM:0200  dmatx_pim2:                  .block 2
RAM:0202  dmatx_tps:                   .block 2
RAM:0204  dmatx_ect:                   .block 2
RAM:0206  dmatx_inj_pw_inj1:           .block 2
RAM:0208  dmatx_pim:                   .block 2
RAM:020A  dmatx_tha:                   .block 1
RAM:020B  dmatx_tham:                  .block 1
RAM:020C  dmatx_battery:               .block 1
RAM:020D  dmatx_nv_trim_pim:           .block 1
RAM:020E  dmatx_cmd_startup_20E:       .block 1
RAM:020F  dmatx_cnt_unk_20F:           .block 1
RAM:0210  dmatx_nv_trim_o2:            .block 1
RAM:0211  dmatx_lambda_state:          .block 1
RAM:0212  dmatx_adc_lambda:            .block 1
RAM:0213  dmatx_knock_retard_info:     .block 3
RAM:0216  dmatx_ign_corr_cpu2:         .block 1
RAM:0217  dmatx_obd_inj:               .block 1
RAM:0218  dmatx_ign_obd:               .block 1
RAM:0219  dmatx_obd_iscv:              .block 1
RAM:021A  dmatx_obd_o2_sensor:         .block 1
RAM:021B  dmatx_knock_retard:          .block 1
RAM:021C  dmatx_pw_loop_mode:          .block 1
RAM:021D  dmatx_tps_delta:             .block 1
RAM:021E  dmatx_error_flags1:          .block 1
RAM:021F  dmatx_error_flags2:          .block 1
RAM:0220  dmatx_flags_46:              .block 1
RAM:0221  dmatx_flags_1:               .block 1
RAM:0222  dmatx_limiter_flags:         .block 1
RAM:0223  unk_223:                     .block 1
RAM:0224  word_224:                    .block 2

RAM:0226  dmarx_word_226:              .block 2
RAM:0228  dmarx_word_228:              .block 2
RAM:022A  dmarx_word_22A:              .block 2
RAM:022C  dmarx_scaled_ve:             .block 2
RAM:022E  dmarx_rpm_x_5p12:            .block 2
RAM:0230  dmarx_warmup_enrich:         .block 1
RAM:0231  dmarx_fuel_trim_231:         .block 1
RAM:0232  dmarx_enrich_232:            .block 1
RAM:0233  dmarx_enrich_233:            .block 1
RAM:0234  dmarx_enrich_unk_234:        .block 1
RAM:0235  dmarx_tham_enrich_unk:       .block 1
RAM:0236  dmarx_idle_enrich:           .block 1
RAM:0237  dmarx_fuel_enrich:           .block 1
RAM:0238  (unnamed):                   .block 1
RAM:0239  dmarx_fuel_ign_corr:         .block 1
RAM:023A  dmarx_knock_retard_cpu2:     .block 1
RAM:023B  dmarx_max_retard_23B_161:    .block 1
RAM:023C  dmarx_lambda_trim:           .block 1
RAM:023D  dmarx_ign_timing:            .block 1
RAM:023E  dmarx_ign_timing_fallback1:  .block 1
RAM:023F  dmarx_ign_timing_fallback2:  .block 1
RAM:0240  dmarx_ign_timing_unk_166:    .block 1
RAM:0241  dmarx_unk_241_167:           .block 1
RAM:0242  dmarx_iscv_duty:             .block 1
RAM:0243  dmarx_status1_169:           .block 1
RAM:0244  damrx_unk_244:               .block 1
RAM:0245  dmarx_status2_16B:           .block 1
RAM:0246  dmarx_ign_advance_hi:        .block 1
RAM:0247  dmarx_ign_advance_lo:        .block 1

RAM:0248  end of the RX block (byte_248 follows)
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

	ECU_Dma.Cpu2ToCpu1 = *(const ECU_DmaData2_t *)RxBuffer;
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

		ECU_CanFrame1001_t Frame1001;
		Frame1001.ECT = EcuData->Ect;
		Frame1001.PIM = EcuData->Pim;
		Frame1001.THA = EcuData->Tha;
		Frame1001.THAM = EcuData->Tham;
		Frame1001.TPS = EcuData->Tps;
		CAN_Tx(TOYOTUNE_CAN_ID_TELEMETRY_1, &Frame1001, sizeof(Frame1001));

		ECU_CanFrame1002_t Frame1002;
		memcpy(Frame1002.KnockBands, EcuData->KnockRetardInfo, 3);
		Frame1002.Knock = EcuData->KnockRetard;
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

