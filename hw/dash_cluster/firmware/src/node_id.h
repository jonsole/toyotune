/*
 * node_id.h
 *
 * Which of the three gauges this board is, read from a resistor divider.
 *
 * WHY NOT A #define
 *
 * The whole point of three independent nodes is that node one is built and
 * debugged, then replicated. That only holds if the binary is genuinely the
 * same: a build-time identity would give three binaries and make flashing the
 * wrong one a real mistake - exactly the hazard CLAUDE.md records for
 * TOYOTUNE_CPU on the Toyotune boards, where it notes a strap would have made
 * one binary universal.
 *
 * WHY AN ADC PIN RATHER THAN TWO DIGITAL STRAPS
 *
 * Two reasons, both from PLAN.md section 4.5:
 *
 *   - Pin budget. The Waveshare board exposes only five GPIOs. One pin
 *     instead of two leaves two spare rather than one.
 *   - It sidesteps erratum RP2350-E9, which affects a GPIO configured as a
 *     digital input with the internal pull-down enabled - such a pin can
 *     latch around 2.2 V and read high when it should read low. An ADC pin in
 *     analogue mode has its digital input buffer disabled, so the erratum
 *     does not apply at all.
 *
 * The levels are spaced across the range rather than packed, so a resistor
 * tolerance or a bit of noise cannot move a node from one identity to the
 * next. Keep the divider impedance low - a few kOhm - so the ADC sees a stiff
 * source.
 */

#ifndef NODE_ID_H_
#define NODE_ID_H_

#include <stdbool.h>
#include <stdint.h>

#define NODE_ID_COUNT		(4)
#define NODE_ID_UNKNOWN		(0xFFu)

/* ADC2 is GPIO28. GPIO26/27 are the other free analogue-capable pins; 29
   carries the AXP2101 interrupt as well and is deliberately avoided. */
#define NODE_ID_ADC_INPUT	(2)

/* Nominal divider outputs, as a fraction of full scale in 1/1000ths.  Four
   identities spread across the range with 250 permille between neighbours,
   so the decision thresholds sit 125 permille from any nominal value. */
extern const uint16_t NodeId_NominalPermille[NODE_ID_COUNT];

/* Convert a raw 12-bit ADC reading into an identity. Separated from the
   hardware so it can be tested on a host across the whole input range,
   including the gaps between levels. Returns NODE_ID_UNKNOWN when the
   reading is not close enough to any nominal level to be trusted. */
extern uint8_t NodeId_FromAdc(uint16_t Raw12Bit);

/* Read the divider and latch the result. Called once at boot: the identity
   cannot change while running, and re-reading it would only add a way for
   noise to move a gauge. */
extern void NodeId_Init(void);

/* The latched identity, or NODE_ID_UNKNOWN if the reading was ambiguous. A
   caller should still start - defaulting to node 0 and saying so beats a
   blank screen - but should make the fault visible. */
extern uint8_t NodeId_Get(void);

#endif /* NODE_ID_H_ */
