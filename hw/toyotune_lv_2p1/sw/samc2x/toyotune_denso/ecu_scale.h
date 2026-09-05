/*
 * ecu_scale.h
 *
 * Raw ECU values -> engineering units.
 *
 * Why this exists, and why the conversion happens on the board rather than in
 * toyotune.dbc: a DBC can only express raw * factor + offset, a straight line.
 * ECT, THA and THAM come from NTC thermistors and are not linear in ADC
 * counts, so no factor/offset can describe them - they need a lookup table,
 * which only code can do.  Once one signal has to be converted here, doing the
 * rest here too keeps every consumer (the dash nodes, can_monitor.py, any
 * logger) agreeing by construction instead of by three copies of the same
 * arithmetic.  The ECU's internal encodings are strange enough - ECT is
 * XOR-inverted and masked, THA/THAM are XOR 0xFF, RPM is x5.12 - that
 * publishing them raw would leak ECU internals into the wire protocol.
 *
 * Nothing is lost for reverse engineering: the RAW telemetry frame still
 * carries every byte of both DMA blocks unmodified.
 *
 * TWO RULES FOR CALLERS
 *
 * 1. Values passed in must already be in HOST byte order.  The Denso CPU is
 *    big-endian and the SAMC21 is not, so every 16-bit field read out of a DMA
 *    block has to go through ECU_Be16() first (see ecu.h).  This module deals
 *    in numbers, not in wire layout, and cannot tell a byte-swapped value from
 *    a correct one.
 *
 * 2. No floating point.  The SAMC21 is a Cortex-M0+ with no FPU, so every
 *    conversion below is fixed-point integer arithmetic.  Do not introduce a
 *    float here "just for clarity" - it pulls in the soft-float library and
 *    costs thousands of cycles.
 *
 * Conversions whose scaling is not yet established - TPS, ignition timing,
 * ISCV duty, the lambda ADC and the trims - are deliberately absent rather
 * than guessed.  Those signals still reach the bus unscaled until their
 * transfer functions are derived; see hw/dash_cluster/PLAN.md sections 3.4
 * and 3.6b.
 */


#ifndef ECU_SCALE_H_
#define ECU_SCALE_H_

#include <stdint.h>


/* Temperature - ECT, THA and THAM, all three from one curve.
 *
 * The argument is the ECU value in Q8 (i.e. value * 256), which is what lets
 * one function serve both widths:
 *
 *     ECT       ECU_ScaleTempC100(Ect)                 - already 16-bit, the
 *                                                        low byte carries two
 *                                                        real extra bits
 *     THA/THAM  ECU_ScaleTempC100((uint16_t)Tha << 8)  - 8-bit, no fraction
 *
 * Note this is the ECU value, the figure after the ECU's own XOR 0xFF - not
 * the raw ADC reading.
 *
 * Returns hundredths of a degree C.  Monotonic across the whole input range.
 * Above an ECU value of about 238 the curve is extrapolating beyond its
 * calibration data and climbs steeply toward a pole; the numbers stay
 * plausible as overheat readings but should be displayed as a warning state
 * rather than trusted as measurements. */
extern int16_t ECU_ScaleTempC100(uint16_t EcuValueQ8);

/* Engine speed.  RpmX5p12 is ECU_DmaData2_t.RpmX5p12, which is rpm * 5.12.
   Returns whole rpm; exact, since 5.12 = 512/100. */
extern uint16_t ECU_ScaleRpm(uint16_t RpmX5p12);

/* Injector 1 pulse width.  The DMA field counts 4us per bit.  Returns
   microseconds, saturating rather than wrapping - real values are far below
   the ceiling, so saturation only ever hides a nonsense input. */
extern uint16_t ECU_ScaleInjPwUs(uint16_t InjPwRaw);

/* Injector duty cycle, in hundredths of a percent.  Derived rather than read:
   both inputs already travel together in the FAST frame, so this costs no bus
   space.  Returns 0 below a plausible idle speed, where the arithmetic is
   meaningless rather than merely imprecise.

   Values above 100% are returned as-is rather than clamped: an injector asked
   for more than 100% duty is a real and important fault condition, and hiding
   it behind a clamp would be worse than showing it. */
extern uint16_t ECU_ScaleInjDutyPct100(uint16_t InjPwUs, uint16_t Rpm);

/* Battery voltage.  Returns hundredths of a volt. */
extern uint16_t ECU_ScaleBatteryV100(uint8_t BatteryRaw);

/* Manifold absolute pressure, from Pim or Pim2.  Returns tenths of a kPa
   ABSOLUTE, and is signed deliberately: at the sensor's zero-pressure clamp
   the conversion lands just below zero (about -1.2 kPa), so an unsigned
   return would wrap to 6553.5 kPa at exactly the point a fault puts it
   there. */
extern int16_t ECU_ScalePimKpa10(uint16_t PimRaw);

/* Knock retard, from KnockRetard, KnockRetardInfo[] or KnockRetardCpu2.
   Returns hundredths of a degree as a POSITIVE magnitude of retard - "4.50"
   means 4.5 degrees pulled out, not added. */
extern int16_t ECU_ScaleRetardDeg100(uint8_t RetardRaw);

#endif /* ECU_SCALE_H_ */
