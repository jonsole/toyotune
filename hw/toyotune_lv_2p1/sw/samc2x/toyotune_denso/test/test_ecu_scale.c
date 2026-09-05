/*
 * test_ecu_scale.c
 *
 * Host unit tests for ecu_scale.c.  Builds and runs natively - no hardware,
 * no target toolchain - so the conversions can be checked against their
 * documented sources every time they are touched.
 *
 * These conversions are the most dangerous code in the telemetry path,
 * because a wrong constant does not crash: it produces a gauge that is
 * plausible and wrong, which is worse than one that is obviously broken.
 * Every assertion below therefore checks against an external source - the
 * spreadsheets under roms/3S-GTE/, the disassembly notes in
 * roms/3S-GTE/gen3/adc_system.md, or physics - rather than against what the
 * implementation happens to produce.
 *
 * Build and run:  python test/run_tests.py
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../ecu_scale.h"
#include "temp_calibration_data.h"

static int Failures = 0;
static int Checks = 0;

#define CHECK(cond, ...)                                                      \
	do {                                                                      \
		Checks++;                                                             \
		if (!(cond)) {                                                        \
			Failures++;                                                       \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__);                     \
			printf(__VA_ARGS__);                                              \
			printf("\n");                                                     \
		}                                                                     \
	} while (0)

#define CHECK_NEAR(got, want, tol, ...)                                       \
	do {                                                                      \
		long g_ = (long)(got), w_ = (long)(want), t_ = (long)(tol);           \
		long d_ = g_ > w_ ? g_ - w_ : w_ - g_;                                \
		Checks++;                                                             \
		if (d_ > t_) {                                                        \
			Failures++;                                                       \
			printf("  FAIL %s:%d: got %ld, want %ld +/- %ld  (", __FILE__,    \
			       __LINE__, g_, w_, t_);                                     \
			printf(__VA_ARGS__);                                              \
			printf(")\n");                                                    \
		}                                                                     \
	} while (0)


/* Integer square root.  These tests deal in integers throughout, and pulling
   in math.h just to report an RMS would be a poor trade. */
static long IntSqrt(long v)
{
	long r = 0;

	while ((r + 1) * (r + 1) <= v)
		r++;

	return r;
}


/***************************************************************************************/
/* Temperature.
 *
 * The first two reference points come from roms/3S-GTE/gen3/adc_system.md,
 * derived from the disassembly as the sensor-fault defaults - independently
 * of the spreadsheet the curve came from, so agreement is real corroboration
 * rather than a tautology. */
static void TestTemperatureReferences(void)
{
	printf("temperature - documented reference points\n");

	/* THA/THAM fault default 0x86 = 134, adc_system.md says about 20 C.
	   8-bit sensors have no fraction, hence the << 8. */
	CHECK_NEAR(ECU_ScaleTempC100((uint16_t)134 << 8), 2000, 100,
	           "THA 0x86 should be ~20 C");

	/* ECT fault default var_ect = 0xE400, adc_system.md says about 82 C.
	   Passed whole: the low byte carries two real bits of resolution. */
	CHECK_NEAR(ECU_ScaleTempC100(0xE400), 8200, 100,
	           "ECT 0xE400 should be ~82 C");

	/* convert.xlsx's own worked cell: ECU value 0xD8 = 216 -> 67.165 C. */
	CHECK_NEAR(ECU_ScaleTempC100((uint16_t)216 << 8), 6717, 5,
	           "convert.xlsx cell says 67.165 C");
}


/* Against the 84 measured bench points.  The tolerance is set by the data,
   not by the curve: the measurements repeat some ECU values with readings up
   to 1.4 C apart, so anything tighter would be asserting noise. */
static void TestTemperatureAgainstBenchData(void)
{
	long WorstErr = 0;
	uint16_t WorstAt = 0;
	long SumSq = 0;
	int i;

	printf("temperature - %d measured bench points\n", TEMP_CALIBRATION_COUNT);

	for (i = 0; i < TEMP_CALIBRATION_COUNT; i++)
	{
		uint16_t X = TempCalibration[i].EcuValue;
		int16_t Want = TempCalibration[i].MeasuredC100;
		int16_t Got = ECU_ScaleTempC100((uint16_t)(X << 8));
		long Err = (long)Got - (long)Want;
		long Abs = Err < 0 ? -Err : Err;

		SumSq += Err * Err;
		if (Abs > WorstErr)
		{
			WorstErr = Abs;
			WorstAt = X;
		}

		CHECK(Abs <= 140, "X=%u measured %d, curve gave %d (err %ld/100 C)",
		      X, Want, Got, Err);
	}

	printf("  worst %ld/100 C at ECU value %u, RMS %ld/100 C\n",
	       WorstErr, WorstAt, IntSqrt(SumSq / TEMP_CALIBRATION_COUNT));

	/* The published fit quality.  If a change makes this materially worse,
	   the curve or the table has drifted. */
	CHECK(WorstErr <= 140, "worst-case error should stay near the 1.21 C fit");
}


/* Monotonic and non-wrapping across every possible input.  A table lookup
   with interpolation is easy to get subtly wrong at a boundary, and the top
   of the range reads the extra entry the generator adds. */
static void TestTemperatureShape(void)
{
	int32_t Prev = -40000;
	uint32_t v;
	int Monotonic = 1;

	printf("temperature - shape across the full input range\n");

	for (v = 0; v <= 0xFFFFu; v++)
	{
		int32_t T = ECU_ScaleTempC100((uint16_t)v);

		if (T < Prev)
		{
			Monotonic = 0;
			printf("  non-monotonic at Q8 input %u: %d after %d\n",
			       (unsigned)v, (int)T, (int)Prev);
			break;
		}
		Prev = T;
	}
	CHECK(Monotonic, "curve must rise monotonically with ECU value");

	/* Endpoints, so an off-by-one in the table indexing shows up. */
	CHECK_NEAR(ECU_ScaleTempC100(0), -3520, 2, "X=0 is a measured point, -34.2 C");
	CHECK_NEAR(ECU_ScaleTempC100(0xFF00), 17267, 2, "X=255 top of table");
}


/***************************************************************************************/
/* Engine speed.  rpm = raw / 5.12 exactly, so this is checked as an identity
   rather than with a tolerance. */
static void TestRpm(void)
{
	printf("engine speed\n");

	CHECK(ECU_ScaleRpm(0) == 0, "zero is zero");
	CHECK(ECU_ScaleRpm(3584) == 700, "3584 / 5.12 = 700 rpm (idle)");
	CHECK(ECU_ScaleRpm(35840) == 7000, "35840 / 5.12 = 7000 rpm");
	CHECK(ECU_ScaleRpm(65535) == 12799, "full scale stays inside uint16");

	/* ecu.h notes the high byte times 50 approximates rpm - a useful
	   independent sanity check on the scale factor. */
	CHECK_NEAR(ECU_ScaleRpm(0x2000), (0x20 * 50), 30,
	           "high byte x 50 should approximate rpm");
}


/***************************************************************************************/
/* Injector pulse width and the duty cycle derived from it. */
static void TestInjector(void)
{
	printf("injector pulse width and duty\n");

	CHECK(ECU_ScaleInjPwUs(0) == 0, "zero is zero");
	CHECK(ECU_ScaleInjPwUs(1000) == 4000, "4us per count");
	CHECK(ECU_ScaleInjPwUs(65535) == 65535, "saturates rather than wrapping");

	/* Physics: at 6000 rpm one cycle of two revolutions is 20ms, so a 10ms
	   pulse is exactly 50% duty. */
	CHECK_NEAR(ECU_ScaleInjDutyPct100(10000, 6000), 5000, 2,
	           "10ms at 6000 rpm is 50%%");
	CHECK_NEAR(ECU_ScaleInjDutyPct100(5000, 6000), 2500, 2,
	           "5ms at 6000 rpm is 25%%");
	CHECK_NEAR(ECU_ScaleInjDutyPct100(2000, 800), 133, 2,
	           "2ms at 800 rpm is about 1.33%%");

	/* Not running: honest zero rather than a divide-by-zero or a huge
	   meaningless number. */
	CHECK(ECU_ScaleInjDutyPct100(5000, 0) == 0, "stopped engine reads zero");
	CHECK(ECU_ScaleInjDutyPct100(5000, 50) == 0, "below cranking reads zero");

	/* Over 100% must be reported, not clamped - it is a real fault. */
	CHECK(ECU_ScaleInjDutyPct100(30000, 6000) > 10000,
	      "over-100%% duty must not be hidden by a clamp");
}


/***************************************************************************************/
/* Battery.  The four fit points are from
   roms/3S-GTE/D151803-9661/battery_voltage_conversion.xlsx. */
static void TestBattery(void)
{
	printf("battery voltage\n");

	CHECK_NEAR(ECU_ScaleBatteryV100(70), 548, 3, "70 -> 5.478 V");
	CHECK_NEAR(ECU_ScaleBatteryV100(119), 925, 3, "119 -> 9.25 V");
	CHECK_NEAR(ECU_ScaleBatteryV100(141), 1100, 3, "141 -> 11.0 V");
	CHECK_NEAR(ECU_ScaleBatteryV100(188), 1460, 3, "188 -> 14.6 V");

	/* The offset matters: without it, zero raw would read 0.00 V rather than
	   the 0.06 V the fit gives, and every reading would sit low. */
	CHECK_NEAR(ECU_ScaleBatteryV100(0), 6, 1, "offset is 0.0601 V");
}


/***************************************************************************************/
/* MAP.  All four rows of the table in roms/3S-GTE/gen3/adc_system.md, which
   are themselves cross-checked there against the ROM's own boost-limit
   comment and its sensor-fault default. */
static void TestPim(void)
{
	printf("manifold pressure\n");

	/* Hard vacuum clamp - about 0 kPa absolute, and legitimately just below
	   zero once the fit's offset is applied.  This is exactly why the return
	   type is signed. */
	CHECK(ECU_ScalePimKpa10(0x0000) < 0,
	      "zero-pressure clamp must go slightly negative, not wrap");
	CHECK_NEAR(ECU_ScalePimKpa10(0x0000), -12, 3, "0.806 V, hard vacuum");

	CHECK_NEAR(ECU_ScalePimKpa10(0x2E4D), 462, 3, "1.51 V, typical hot idle");
	CHECK_NEAR(ECU_ScalePimKpa10(0x6443), 1013, 3, "2.33 V, atmospheric");
	CHECK_NEAR(ECU_ScalePimKpa10(0xDA00), 2217, 4, "4.12 V, 17.5 psi boost cut");

	/* Atmospheric should land on atmospheric - the single most recognisable
	   value in the whole conversion, and the one a sign or scale error would
	   move first. */
	CHECK_NEAR(ECU_ScalePimKpa10(0x6443), 1013, 5,
	           "atmospheric must read ~101.3 kPa");
}


/***************************************************************************************/
static void TestKnockRetard(void)
{
	printf("knock retard\n");

	CHECK(ECU_ScaleRetardDeg100(0) == 0, "no retard is zero");
	CHECK(ECU_ScaleRetardDeg100(2) == 100, "0.5 deg per count");
	CHECK(ECU_ScaleRetardDeg100(20) == 1000, "20 counts is 10 degrees");
	CHECK(ECU_ScaleRetardDeg100(255) == 12750, "full scale stays in int16");
	CHECK(ECU_ScaleRetardDeg100(10) > 0, "retard is a positive magnitude");
}


/***************************************************************************************/
int main(void)
{
	printf("ecu_scale unit tests\n");
	printf("--------------------\n");

	TestTemperatureReferences();
	TestTemperatureAgainstBenchData();
	TestTemperatureShape();
	TestRpm();
	TestInjector();
	TestBattery();
	TestPim();
	TestKnockRetard();

	printf("--------------------\n");
	printf("%d checks, %d failures\n", Checks, Failures);
	return Failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
