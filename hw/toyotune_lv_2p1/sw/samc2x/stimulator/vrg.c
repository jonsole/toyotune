/*
 * vrg.c
 *
 * Created: 07/02/2019 18:56:45
 *  Author: WinUser
 */ 

#include <sam.h>

#include "debug.h"
#include "clk.h"
#include "pio.h"
#include "vrg.h"

/*
 * Signal generation, and where each signal appears on the SAM C21 Xplained Pro.
 *
 * NE, G1 and G2 are each a PAIR of outputs, not a single line. The two are
 * summed into a single ECU input rather than driven differentially, so between
 * them they produce three voltage levels - see the wiring below. All seven
 * lines come from TCC0's pattern generator and update on the same overflow, so
 * they stay in exact phase with one another.
 *
 *   Signal        Pattern  TCC0  MCU pin  Xplained Pro header     Idle
 *   NE  high half  PGV1    WO1   PB31     EXT2 pin 8  PWM(-)      low
 *   NE  low half   PGV0    WO0   PB30     EXT2 pin 7  PWM(+)      high
 *   G1  high half  PGV3    WO3   PA19     EXT1 pin 18 SPI_SCK     low
 *   G1  low half   PGV2    WO2   PA18     EXT1 pin 16 SPI_MOSI    high
 *   G2  high half  PGV5    WO5   PB17     EXT2 pin 10 SPI_SS_B    low
 *   G2  low half   PGV4    WO4   PB16     EXT2 pin 9  IRQ/GPIO    high
 *   TDC marker     PGV6    WO6   PA20     EXT1 pin 5  GPIO1       low
 *
 * PB30/PB31 also appear on the Arduino headers as D6 (J804 pin 7) and D9
 * (J801 pin 2). PB16 and PB17 are shared with EDBG GPIO1/GPIO2, so they are
 * not free pins even though nothing drives them from the debugger side.
 *
 * Wiring. Each pair is summed through two 1k resistors and AC-coupled into the
 * ECU through 0.1uF. NE shown; G1 and G2 are the same arrangement on their own
 * pins:
 *
 *      PB31 (WO1) ---[ 1k ]---+
 *                             |     0.1uF
 *                             +------| |------>  ECU NE input
 *                             |
 *      PB30 (WO0) ---[ 1k ]---+
 *
 * The junction sits at the AVERAGE of the pair, not the difference, which is
 * what gives three levels: 0V both low, 3.3V both high, 1.65V one of each.
 * 1.65V is the resting level the coupling capacitor strips off, so it becomes
 * the zero the ECU sees. Source impedance into the capacitor is 1k||1k = 500R.
 *
 * Only the NE network has been confirmed directly. G1 and G2 are assumed
 * identical, since their three-level pattern only makes sense against the same
 * summing network.
 *
 * Timing. GCLK1 is 1MHz and the period is 1000000 / (Rpm * 48 / 60), so TCC0
 * overflows 48 times per crankshaft revolution and one pattern slot is 7.5
 * degrees of crank. The 96-slot pattern is therefore 720 degrees - one full
 * engine cycle - which gives:
 *
 *   NE   level change every 2 slots = one zero crossing per 15 deg crank,
 *        so 24 per crank revolution - the geometry the ECU expects
 *   G1   at slot 95 and G2 at slot 47, i.e. 48 slots = 360 deg crank apart
 *   TDC  slots 0 to 2   = 22.5 deg, spanning 10 BTDC to 12.5 ATDC
 *
 * The TDC marker on PA20 is the reference for the ignition timing
 * measurement: loop it back to PB09 (EXT1 pin 3), which igt.c samples as IGT
 * Sync. The ECU's own IGT output comes in on PB08 (EXT1 pin 4).
 *
 * Header pin assignments are from the Atmel SAM C21 Xplained Pro User Guide,
 * Atmel-42460D 08/2016, tables 4-1 (EXT1), 4-2 (EXT2), 4-6 and 4-7 (Arduino).
 */

#define VRG_PATTERN_SIZE (24 * 4)
static uint32_t VRG_Pattern[VRG_PATTERN_SIZE];
static uint16_t VRG_Rpm;

/* NE, G1 and G2 each drive a PAIR of outputs that are summed through two 1k
   resistors and AC-coupled into the ECU through 0.1uF. The junction therefore
   sits at the AVERAGE of the pair, not the difference:

       both low    -> 0V        both high -> 3.3V
       one of each -> 1.65V, the AC-coupled baseline

   So the two pins carrying the SAME level is what produces full swing, and a
   complementary pair would hold the junction at a constant 1.65V - no signal
   at all once the coupling capacitor has removed the DC.

   G1 and G2 use all three levels: baseline, then 3.3V, then 0V, then back to
   baseline, which is the positive-then-negative shape of a variable-reluctance
   pickup. NE repeats every four slots and spends two high and two low, so
   unlike G1/G2 it has no room for a baseline rest between teeth - its junction
   is a square wave alternating 0V and 3.3V.

   What the ECU counts is the ZERO CROSSING, not the cycle. The junction
   changes level every two slots, so there are 48 / 2 = 24 crossings per
   crankshaft revolution, 15 degrees apart - which is exactly the crank
   geometry the ECU firmware expects (roms/3S-GTE/gen3/ecu_overview.md: "24 NE
   pulses/rev, 15 deg apart"). Each crossing is one tooth, and ignition timing
   is scheduled from it, so the edge wants to be clean and its timing exact.
   Both NE pins carry the same pattern and are updated by the same TCC
   overflow, so they switch together and the junction steps 0V to 3.3V with no
   intermediate 1.65V plateau to slow the crossing down. */
static void VRG_PatternAddNe(int Index)
{
	VRG_Pattern[(VRG_PATTERN_SIZE + Index - 2) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV1;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index - 1) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV1;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 0) % VRG_PATTERN_SIZE] &= ~TCC_PATT_PGV0;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 1) % VRG_PATTERN_SIZE] &= ~TCC_PATT_PGV0;
}

static void VRG_PatternAddG1(int Index)
{
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 0) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV3;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 1) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV3;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 2) % VRG_PATTERN_SIZE] &= ~TCC_PATT_PGV2;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 3) % VRG_PATTERN_SIZE] &= ~TCC_PATT_PGV2;
}

static void VRG_PatternAddG2(int Index)
{
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 0) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV5;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 1) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV5;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 2) % VRG_PATTERN_SIZE] &= ~TCC_PATT_PGV4;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 3) % VRG_PATTERN_SIZE] &= ~TCC_PATT_PGV4;
}

static void VRG_PatternAddBtdc(int Index)
{
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 0) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV6;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 1) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV6;
	VRG_Pattern[(VRG_PATTERN_SIZE + Index + 2) % VRG_PATTERN_SIZE] |= TCC_PATT_PGV6;
}

void VRG_PatternInit(void)
{
	for (int Index = 0; Index < VRG_PATTERN_SIZE; Index++)
	{
		VRG_Pattern[Index] = TCC_PATT_PGV0 | TCC_PATT_PGV2 | TCC_PATT_PGV4 |
		TCC_PATT_PGE0 | TCC_PATT_PGE1 | TCC_PATT_PGE2 |
		TCC_PATT_PGE3 | TCC_PATT_PGE4 | TCC_PATT_PGE5 |
		TCC_PATT_PGE6;
	}
	for (int Index = 0; Index < 96; Index += 4)
	{
		VRG_PatternAddNe(Index);
	}
	VRG_PatternAddG1(-1);
	VRG_PatternAddG2((12 * 4) - 1);

	/* Set 10 BTDC (high) 12.5 ATDC (low) marker */
	VRG_PatternAddBtdc(0);
}

void VRG_SetRpm(uint16_t Rpm)
{
	VRG_Rpm = Rpm;
}

uint16_t VRG_GetRpm(void)
{
	return VRG_Rpm;
}


static uint32_t VRG_CalcPerBufValue(uint16_t Rpm)
{
	return 1000000UL / (VRG_Rpm * 48 / 60);
}

void TCC0_Handler(void)  __attribute__((__interrupt__));
void TCC0_Handler(void)
{
	static int Index = 0;

	while(TCC0->SYNCBUSY.reg  & TCC_SYNCBUSY_PATT);
	TCC0->PATTBUF.reg = VRG_Pattern[Index];
	Index = (Index + 1) % (sizeof(VRG_Pattern) / sizeof(uint32_t));

	/* Clear OVF interrupt */
	TCC0->INTFLAG.reg = TCC_INTFLAG_OVF;

	/* Update period from RPM */
	TCC0->PERBUF.reg = VRG_CalcPerBufValue(VRG_Rpm);
}

void VRG_Init(void)
{
	VRG_PatternInit();

	/* Initialize GPIO (PORT) */
	PIO_SetPeripheral(PIN_PB30, PIO_PERIPHERAL_E);
	PIO_EnablePeripheral(PIN_PB30);				/* NE  low half  - WO0, EXT2 pin 7 */
	PIO_SetPeripheral(PIN_PB31, PIO_PERIPHERAL_E);
	PIO_EnablePeripheral(PIN_PB31);				/* NE  high half - WO1, EXT2 pin 8 */
	PIO_SetPeripheral(PIN_PA18, PIO_PERIPHERAL_F);
	PIO_EnablePeripheral(PIN_PA18);				/* G1  low half  - WO2, EXT1 pin 16 */
	PIO_SetPeripheral(PIN_PA19, PIO_PERIPHERAL_F);
	PIO_EnablePeripheral(PIN_PA19);				/* G1  high half - WO3, EXT1 pin 18 */
	PIO_SetPeripheral(PIN_PB16, PIO_PERIPHERAL_F);
	PIO_EnablePeripheral(PIN_PB16);				/* G2  low half  - WO4, EXT2 pin 9 */
	PIO_SetPeripheral(PIN_PB17, PIO_PERIPHERAL_F);
	PIO_EnablePeripheral(PIN_PB17);				/* G2  high half - WO5, EXT2 pin 10 */
	PIO_SetPeripheral(PIN_PA20, PIO_PERIPHERAL_F);
	PIO_EnablePeripheral(PIN_PA20);					/* TDC marker  - WO6, EXT1 pin 5,
													   10 BTDC - 12.5 ATDC */

	/* Enable TCC0 Bus clock */
	MCLK->APBCMASK.reg |= MCLK_APBCMASK_TCC0;

	/* Enable 1MHz GCLK1 for TCC0 */
	GCLK->PCHCTRL[TCC0_GCLK_ID].reg &= ~GCLK_PCHCTRL_CHEN;
	while (GCLK->PCHCTRL[TCC0_GCLK_ID].reg & GCLK_PCHCTRL_CHEN);
	GCLK->PCHCTRL[TCC0_GCLK_ID].reg = GCLK_PCHCTRL_GEN_GCLK1;
	GCLK->PCHCTRL[TCC0_GCLK_ID].reg |= GCLK_PCHCTRL_CHEN;
	while (!(GCLK->PCHCTRL[TCC0_GCLK_ID].reg & GCLK_PCHCTRL_CHEN));

	/* Initialize TCC0 */
	TCC0->CTRLA.reg &=~(TCC_CTRLA_ENABLE);
	TCC0->WAVE.reg |= TCC_WAVE_WAVEGEN_NFRQ;

	/* Enable update interrupt */
	TCC0->INTENSET.reg |= TCC_INTENSET_OVF;
	NVIC_SetPriority(TCC0_IRQn, 1);
	NVIC_EnableIRQ(TCC0_IRQn);

	/* Set initial RPM */
	VRG_SetRpm(900);
	TCC0->PERBUF.reg = TCC0->PER.reg = VRG_CalcPerBufValue(VRG_Rpm);

	/* Enable TCC0 */
	TCC0->CTRLA.reg |= TCC_CTRLA_ENABLE;
}


