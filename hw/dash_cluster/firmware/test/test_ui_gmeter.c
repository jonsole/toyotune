/*
 * test_ui_gmeter.c - host tests for the g-force meter's arithmetic.
 *
 * The axis conventions and the zeroing are the parts that can be wrong while
 * looking right - a dot that moves the wrong way in a bend, or a tilted mount
 * that reads braking as a lean - so they are checked here with the car's
 * motion constructed in the sensor's own axes.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_gmeter.h"

extern int GMeterTests_Run(int *Checks, int *Failures);

static int Checks;
static int Failures;

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

#define NEAR(a, b)	(fabs((double)(a) - (double)(b)) < 0.01)

/* Feed one reading for long enough that the smoothing has settled. */
static void Settle(GMeter_t *G, const GMeterCal_t *Cal, int32_t X, int32_t Y, int32_t Z)
{
	GMeterMg_t R;
	int i;

	R.X = X;
	R.Y = Y;
	R.Z = Z;
	for (i = 0; i < 300; i++)
		GMeter_Update(G, Cal, &R, 16667u);
}


/***************************************************************************************/
/* Upright, as assumed before zeroing: +Y is right, +Z is ahead. */
static void TestUpright(void)
{
	GMeter_t G;
	GMeterCal_t Cal;

	GMeter_DefaultCal(&Cal);
	CHECK(!Cal.Zeroed, "the default is an assumption, not a zeroing");

	GMeter_Init(&G);
	Settle(&G, &Cal, 1000, 0, 0);
	CHECK(NEAR(G.Lat, 0.0) && NEAR(G.Lon, 0.0), "at rest reads zero: %.3f, %.3f",
	      (double)G.Lat, (double)G.Lon);

	GMeter_Init(&G);
	Settle(&G, &Cal, 1000, 500, 0);
	CHECK(NEAR(G.Lat, 0.5) && NEAR(G.Lon, 0.0),
	      "0.5 g along +Y is 0.5 g to the right: %.3f, %.3f", (double)G.Lat, (double)G.Lon);

	GMeter_Init(&G);
	Settle(&G, &Cal, 1000, 0, 300);
	CHECK(NEAR(G.Lon, 0.3) && NEAR(G.Lat, 0.0),
	      "0.3 g along +Z is accelerating: %.3f, %.3f", (double)G.Lat, (double)G.Lon);

	GMeter_Init(&G);
	Settle(&G, &Cal, 1000, -250, -800);
	CHECK(NEAR(G.Lat, -0.25) && NEAR(G.Lon, -0.8),
	      "left and braking are negative: %.3f, %.3f", (double)G.Lat, (double)G.Lon);
}


/***************************************************************************************/
/* A mount tilted back 30 degrees: gravity no longer lies along one axis, and
   the car's ahead is not the sensor's +Z. Motion is built from the true
   horizontal directions, which is what the zeroing has to recover. */
static void TestTiltedMount(void)
{
	GMeter_t G;
	GMeterCal_t Cal;
	const double C = cos(30.0 * 3.14159265358979 / 180.0);
	const double S = sin(30.0 * 3.14159265358979 / 180.0);
	/* Up in sensor axes, and ahead: square to up and to +Y. */
	const double Up[3] = { C, 0.0, -S };
	const double Ahead[3] = { S, 0.0, C };
	int32_t X, Y, Z;

	Cal.Rest.X = (int32_t)lround(1000.0 * Up[0]);
	Cal.Rest.Y = 0;
	Cal.Rest.Z = (int32_t)lround(1000.0 * Up[2]);
	Cal.Zeroed = true;

	GMeter_Init(&G);
	Settle(&G, &Cal, Cal.Rest.X, Cal.Rest.Y, Cal.Rest.Z);
	CHECK(NEAR(G.Lat, 0.0) && NEAR(G.Lon, 0.0), "tilted, at rest reads zero");

	/* Braking at 0.6 g: the car's acceleration points backwards. */
	X = Cal.Rest.X + (int32_t)lround(-600.0 * Ahead[0]);
	Y = Cal.Rest.Y;
	Z = Cal.Rest.Z + (int32_t)lround(-600.0 * Ahead[2]);
	GMeter_Init(&G);
	Settle(&G, &Cal, X, Y, Z);
	CHECK(NEAR(G.Lon, -0.6) && NEAR(G.Lat, 0.0),
	      "tilted, braking reads as braking and nothing else: %.3f, %.3f",
	      (double)G.Lat, (double)G.Lon);

	/* And a right-hand bend is still to the right. */
	GMeter_Init(&G);
	Settle(&G, &Cal, Cal.Rest.X, 400, Cal.Rest.Z);
	CHECK(NEAR(G.Lat, 0.4) && NEAR(G.Lon, 0.0),
	      "tilted, a right-hand bend reads right: %.3f, %.3f", (double)G.Lat, (double)G.Lon);
}


/***************************************************************************************/
/* Flat on the bench, display up: +Z points straight down, so "ahead" cannot be
   +Z. It becomes the top of the display, and the meter still works. */
static void TestFlatOnTheBench(void)
{
	GMeter_t G;
	GMeterCal_t Cal;

	GMeter_DefaultCal(&Cal);
	GMeter_Init(&G);
	Settle(&G, &Cal, 35, 60, -982);		/* measured, lying flat */
	CHECK(GMeter_Zero(&G, &Cal), "a flat board can be zeroed");

	GMeter_Init(&G);
	Settle(&G, &Cal, 35 + 300, 60, -982);
	CHECK(NEAR(G.Lon, 0.3) && NEAR(G.Lat, 0.0),
	      "flat, pushing towards the display's top reads as accelerating: %.3f, %.3f",
	      (double)G.Lat, (double)G.Lon);

	GMeter_Init(&G);
	Settle(&G, &Cal, 35, 60 + 200, -982);
	CHECK(NEAR(G.Lat, 0.2) && NEAR(G.Lon, 0.0),
	      "flat, towards the display's right reads right: %.3f, %.3f",
	      (double)G.Lat, (double)G.Lon);
}


/***************************************************************************************/
static void TestZeroing(void)
{
	GMeter_t G;
	GMeterCal_t Cal;

	/* The board's real at-rest reading, from the bench: offsets included. */
	GMeter_DefaultCal(&Cal);
	GMeter_Init(&G);
	Settle(&G, &Cal, 1006, 103, 4);
	CHECK(!NEAR(G.Lat, 0.0), "unzeroed, the sensor's offset shows: %.3f", (double)G.Lat);

	CHECK(GMeter_Zero(&G, &Cal), "a car at rest can be zeroed");
	CHECK(Cal.Zeroed, "and says so");
	CHECK(abs(Cal.Rest.X - 1006) <= 2 && abs(Cal.Rest.Y - 103) <= 2 && abs(Cal.Rest.Z - 4) <= 2,
	      "the zeroing is the at-rest reading: %d,%d,%d",
	      (int)Cal.Rest.X, (int)Cal.Rest.Y, (int)Cal.Rest.Z);

	GMeter_Init(&G);
	Settle(&G, &Cal, 1006, 103, 4);
	CHECK(NEAR(G.Lat, 0.0) && NEAR(G.Lon, 0.0),
	      "zeroed, the offsets are gone: %.3f, %.3f", (double)G.Lat, (double)G.Lon);

	/* Not a car standing still: a reading far from 1 g is refused, and the
	   zeroing already held is kept. */
	{
		GMeterCal_t Before = Cal;

		GMeter_Init(&G);
		Settle(&G, &Cal, 400, 200, 100);
		CHECK(!GMeter_Zero(&G, &Cal), "half a g is not level ground");
		CHECK(memcmp(&Before, &Cal, sizeof(Cal)) == 0, "a refused zeroing changes nothing");
	}

	/* Nothing yet to average. */
	GMeter_Init(&G);
	CHECK(!GMeter_Zero(&G, &Cal), "no samples, no zeroing");
}


/***************************************************************************************/
static void TestPeaksAndTrail(void)
{
	GMeter_t G;
	GMeterCal_t Cal;

	GMeter_DefaultCal(&Cal);
	GMeter_Init(&G);

	Settle(&G, &Cal, 1000, 700, 0);
	Settle(&G, &Cal, 1000, -300, 0);
	Settle(&G, &Cal, 1000, 0, 450);
	Settle(&G, &Cal, 1000, 0, -900);
	Settle(&G, &Cal, 1000, 0, 0);

	CHECK(NEAR(G.PeakRight, 0.7) && NEAR(G.PeakLeft, 0.3), "lateral peaks held: %.3f %.3f",
	      (double)G.PeakRight, (double)G.PeakLeft);
	CHECK(NEAR(G.PeakAccel, 0.45) && NEAR(G.PeakBrake, 0.9), "longitudinal peaks held: %.3f %.3f",
	      (double)G.PeakAccel, (double)G.PeakBrake);

	GMeter_ResetPeaks(&G);
	CHECK(G.PeakRight == 0.0f && G.PeakLeft == 0.0f && G.PeakAccel == 0.0f
	      && G.PeakBrake == 0.0f, "reset clears every peak");

	CHECK(G.TrailCount == GMETER_TRAIL_POINTS, "the trail fills to %u points, has %u",
	      (unsigned)GMETER_TRAIL_POINTS, (unsigned)G.TrailCount);
	CHECK(NEAR(G.TrailLat[G.TrailCount - 1u], 0.0) && NEAR(G.TrailLon[G.TrailCount - 1u], 0.0),
	      "the newest trail point is where the dot is");
}


/***************************************************************************************/
int GMeterTests_Run(int *OutChecks, int *OutFailures)
{
	Checks = 0;
	Failures = 0;

	printf("g-force - axes, a tilted mount, zeroing, peaks and trail\n");
	TestUpright();
	TestTiltedMount();
	TestFlatOnTheBench();
	TestZeroing();
	TestPeaksAndTrail();

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
