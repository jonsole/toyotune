/*
 * ecu.h
 *
 * The ECU's inter-CPU DMA blocks, and access to the latest capture of each.
 *
 * The two blocks are described here rather than in main.c because the CAN
 * telemetry tables are driven off offsetof() into these structs, and because
 * a new ECU family with a different DMA layout is added by adding a branch
 * below rather than by touching any code.
 */


#ifndef ECU_H_
#define ECU_H_

#include <stdint.h>
#include <stdbool.h>

#include "config.h"


/* --------------------------------------------------------------------------
   Per-ECU DMA block layouts.

   The MR2 (D151803-9651/-9661) and ST205 (D151804-0461/-0471) pairs share
   both the geometry and the field slots, differing only in base address, so
   one set of definitions serves both.  They are guarded together rather than
   duplicated because two identical tables would drift apart.

   A family whose layout genuinely differs gets its own #elif branch here,
   with its own structs and its own signal table in can_telemetry.c.  The
   #else makes an unsupported selection a build failure rather than a silent
   mis-parse.
   ------------------------------------------------------------------------ */

#if defined(TOYOTUNE_ECU_MR2) || defined(TOYOTUNE_ECU_ST205)

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


#else
#error "No DMA block layout defined for the selected ECU - add a branch in ecu.h."
#endif


extern bool ECU_GetDmaSnapshot(ECU_DmaData1_t *Cpu1ToCpu2, ECU_DmaData2_t *Cpu2ToCpu1);

#endif /* ECU_H_ */
