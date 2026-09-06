/*
 * node_id.c
 *
 * Reading the identity divider.
 *
 * The decode is deliberately separate from the ADC so it can be tested across
 * every possible reading on a host - including the gaps between levels, which
 * is where a resistor tolerance or a noisy rail would actually put a board,
 * and where an accepting-too-much threshold would silently give a node the
 * wrong identity.
 */

#include "node_id.h"

#if defined(PICO_ON_DEVICE)
#include "hardware/adc.h"
#endif

#define ADC_FULL_SCALE		(4095u)		/* 12-bit */

/* 1/8, 3/8, 5/8, 7/8 of full scale. Even spacing with the extremes kept off
   the rails, so neither a ground offset nor a supply droop can push an
   identity past a threshold. A plain divider of two equal-decade resistors
   hits these closely enough. */
const uint16_t NodeId_NominalPermille[NODE_ID_COUNT] = { 125, 375, 625, 875 };

/* Half the spacing would be 125; accepting only 90 leaves a deliberate dead
   band between levels. A reading that lands in the gap is reported as unknown
   rather than rounded to the nearest identity - two nodes silently claiming
   the same identity is a far worse failure than one node reporting that its
   strap is wrong. */
#define NODE_ID_TOLERANCE_PERMILLE	(90)

static uint8_t LatchedId = NODE_ID_UNKNOWN;


/***************************************************************************************/
uint8_t NodeId_FromAdc(uint16_t Raw12Bit)
{
	uint32_t Permille;
	uint8_t i;

	if (Raw12Bit > ADC_FULL_SCALE)
		Raw12Bit = ADC_FULL_SCALE;

	Permille = ((uint32_t)Raw12Bit * 1000u + (ADC_FULL_SCALE / 2u)) / ADC_FULL_SCALE;

	for (i = 0; i < NODE_ID_COUNT; i++)
	{
		uint32_t Nominal = NodeId_NominalPermille[i];
		uint32_t Delta = (Permille > Nominal) ? (Permille - Nominal)
		                                      : (Nominal - Permille);

		if (Delta <= NODE_ID_TOLERANCE_PERMILLE)
			return i;
	}

	return NODE_ID_UNKNOWN;
}


/***************************************************************************************/
void NodeId_Init(void)
{
#if defined(PICO_ON_DEVICE)
	uint32_t Sum = 0;
	uint8_t i;

	adc_init();
	adc_gpio_init(26 + NODE_ID_ADC_INPUT);
	adc_select_input(NODE_ID_ADC_INPUT);

	/* Average a handful of conversions. The divider is a static voltage, so
	   this costs nothing at boot and removes any single noisy sample from a
	   decision the node then lives with until it is power cycled. */
	for (i = 0; i < 16; i++)
		Sum += adc_read();

	LatchedId = NodeId_FromAdc((uint16_t)(Sum / 16u));
#else
	/* Host builds have no ADC; tests drive NodeId_FromAdc() directly. */
	LatchedId = NODE_ID_UNKNOWN;
#endif
}


/***************************************************************************************/
uint8_t NodeId_Get(void)
{
	return LatchedId;
}
