/*
 * test_ui_needle.c - host tests for the needle rasteriser.
 *
 * The needle is drawn hard-edged into a byte buffer with nobody looking at the
 * result until it is on the glass, so the properties that would otherwise be
 * found by squinting are checked here: its area, that it stays inside the
 * rectangle the renderer sends, that it clears the centre ring, that it is
 * symmetric, and that clipping never writes outside the buffer.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ui_gauge.h"
#include "ui_model.h"
#include "ui_needle.h"

extern int NeedleTests_Run(int *Checks, int *Failures);

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

/* The panel, and the dial as the firmware places it: 447 px at 9,9. */
#define W		(466)
#define H		(466)
#define DIAL_X		(9)
#define DIAL_SIZE	(447)
#define INDEX		(0xA5u)

static uint8_t Buf[H * W];

static UiNeedle_t Place(uint32_t PositionQ)
{
	return UiNeedle_Place((float)DIAL_X + (float)DIAL_SIZE / 2.0f,
	                      (float)DIAL_X + (float)DIAL_SIZE / 2.0f,
	                      (float)UiGauge_NeedleInner(DIAL_SIZE, DIAL_SIZE),
	                      (float)UiGauge_NeedleOuter(DIAL_SIZE, DIAL_SIZE),
	                      (float)UI_GAUGE_NEEDLE_WIDTH / 2.0f, PositionQ);
}

static uint32_t Q(uint32_t Permille)
{
	return Permille << UI_NEEDLE_Q;
}


/***************************************************************************************/
/* Area, containment and ring clearance, at positions round the whole sweep. */
static void TestShape(void)
{
	float Pivot = (float)DIAL_X + (float)DIAL_SIZE / 2.0f;
	float Inner = (float)UiGauge_NeedleInner(DIAL_SIZE, DIAL_SIZE);
	float Outer = (float)UiGauge_NeedleOuter(DIAL_SIZE, DIAL_SIZE);
	float Half = (float)UI_GAUGE_NEEDLE_WIDTH / 2.0f;
	float Area = ((Outer - Inner) * 2.0f * Half) + (3.14159265f * Half * Half);
	uint32_t P;

	for (P = 0; P <= UI_POSITION_MAX; P += 25u)
	{
		UiNeedle_t N = Place(Q(P));
		UiRect_t R = UiNeedle_Bounds(&N);
		uint32_t Set, Counted = 0, Outside = 0, InRing = 0;
		int32_t X, Y;

		memset(Buf, 0, sizeof(Buf));
		Set = UiNeedle_Draw(&N, Buf, W, W, H, INDEX);

		for (Y = 0; Y < H; Y++)
		{
			for (X = 0; X < W; X++)
			{
				float Dx, Dy;

				if (Buf[(Y * W) + X] != INDEX)
					continue;
				Counted++;
				if (X < R.X1 || X > R.X2 || Y < R.Y1 || Y > R.Y2)
					Outside++;

				/* The ring's outer edge is where the needle's rounded inner end
				   stops, so no pixel centre may be nearer the pivot than that. */
				Dx = ((float)X + 0.5f) - Pivot;
				Dy = ((float)Y + 0.5f) - Pivot;
				if (sqrtf((Dx * Dx) + (Dy * Dy)) < (Inner - Half) - 0.01f)
					InRing++;
			}
		}

		CHECK(Set == Counted, "position %u: reported %u pixels, buffer has %u",
		      P, Set, Counted);
		CHECK(fabsf((float)Set - Area) < Area * 0.08f,
		      "position %u: %u pixels, a %.0f px stroke should cover about %.0f",
		      P, Set, (double)(Outer - Inner), (double)Area);
		CHECK(Outside == 0, "position %u: %u pixels outside the bounds sent", P, Outside);
		CHECK(InRing == 0, "position %u: %u pixels inside the centre ring", P, InRing);

		/* The bounds are no looser than a pixel each side - they set how much
		   is sent to the panel every frame. */
		{
			int32_t MinX = W, MinY = H, MaxX = -1, MaxY = -1;

			for (Y = 0; Y < H; Y++)
				for (X = 0; X < W; X++)
					if (Buf[(Y * W) + X] == INDEX)
					{
						if (X < MinX) MinX = X;
						if (X > MaxX) MaxX = X;
						if (Y < MinY) MinY = Y;
						if (Y > MaxY) MaxY = Y;
					}
			CHECK(MinX - R.X1 <= 1 && R.X2 - MaxX <= 1 && MinY - R.Y1 <= 1
			      && R.Y2 - MaxY <= 1,
			      "position %u: bounds %d,%d-%d,%d are loose round %d,%d-%d,%d",
			      P, R.X1, R.Y1, R.X2, R.Y2, MinX, MinY, MaxX, MaxY);
		}
	}
}


/***************************************************************************************/
/* Pointing straight up, the needle is a mirror image of itself about the
   pivot's column - a check on the half-pixel pivot and the centre test. */
static void TestSymmetry(void)
{
	UiNeedle_t N = Place(Q(UI_POSITION_MAX / 2u));	/* 135 + 135 = 270: up */
	int32_t X, Y;
	uint32_t Asym = 0;

	memset(Buf, 0, sizeof(Buf));
	(void)UiNeedle_Draw(&N, Buf, W, W, H, INDEX);

	/* Pivot at 232.5, so column x mirrors to 464 - x. */
	for (Y = 0; Y < H; Y++)
		for (X = 0; X <= 464; X++)
			if ((Buf[(Y * W) + X] == INDEX) != (Buf[(Y * W) + (464 - X)] == INDEX))
				Asym++;

	CHECK(Asym == 0, "an upright needle should be symmetric, %u pixels are not", Asym);
	CHECK(N.X1 > 232.4f && N.X1 < 232.6f && N.Y1 < N.Y0,
	      "half scale should point straight up, tip at %.2f,%.2f",
	      (double)N.X1, (double)N.Y1);
}


/***************************************************************************************/
/* The ends of the sweep land where the graduations are. */
static void TestEnds(void)
{
	UiNeedle_t Lo = Place(0);
	UiNeedle_t Hi = Place(Q(UI_POSITION_MAX));

	/* 135 degrees - lower left - and 405 - lower right. */
	CHECK(Lo.X1 < Lo.X0 && Lo.Y1 > Lo.Y0, "zero should point to the lower left");
	CHECK(Hi.X1 > Hi.X0 && Hi.Y1 > Hi.Y0, "full scale should point to the lower right");
	CHECK(fabsf((Lo.X1 - 232.5f) + (Hi.X1 - 232.5f)) < 0.01f
	      && fabsf(Lo.Y1 - Hi.Y1) < 0.01f,
	      "the two ends should mirror each other");
}


/***************************************************************************************/
static void TestSame(void)
{
	UiNeedle_t A = Place(Q(300));
	UiNeedle_t B = Place(Q(300));
	UiNeedle_t C = Place(Q(300) + 1u);
	UiNeedle_t D = Place(Q(301));

	CHECK(UiNeedle_Same(&A, &B), "identical positions are the same");
	CHECK(UiNeedle_Same(&A, &C), "a 1/256 permille step moves the tip 0.004 px - the same");
	CHECK(!UiNeedle_Same(&A, &D), "a permille moves the tip 0.9 px - not the same");
}


/***************************************************************************************/
/* Drawn into a buffer smaller than the needle's reach, nothing lands outside
   it. The guard bytes either side would catch a stray row or column. */
static void TestClipping(void)
{
	enum { SW = 40, SH = 30, GUARD = 64 };
	static uint8_t Small[GUARD + (SW * SH) + GUARD];
	UiNeedle_t N;
	uint32_t i, Set;
	bool Clean = true;

	/* A needle starting inside the buffer and running well off its edge. */
	N.X0 = 20.0f;
	N.Y0 = 15.0f;
	N.X1 = 120.0f;
	N.Y1 = -60.0f;
	N.HalfWidth = 2.5f;

	memset(Small, 0x11, sizeof(Small));
	Set = UiNeedle_Draw(&N, Small + GUARD, SW, SW, SH, INDEX);

	for (i = 0; i < GUARD; i++)
		if (Small[i] != 0x11 || Small[GUARD + (SW * SH) + i] != 0x11)
			Clean = false;

	CHECK(Clean, "clipped needle wrote outside its buffer");
	CHECK(Set > 0u, "the part inside the buffer should still be drawn");
}


/***************************************************************************************/
static void TestUnion(void)
{
	UiRect_t A = { 10, 20, 30, 40 };
	UiRect_t B = { 5, 25, 35, 30 };
	UiRect_t U = UiRect_Union(&A, &B);

	CHECK(U.X1 == 5 && U.Y1 == 20 && U.X2 == 35 && U.Y2 == 40,
	      "union should be 5,20-35,40, got %d,%d-%d,%d", U.X1, U.Y1, U.X2, U.Y2);
}


/***************************************************************************************/
int NeedleTests_Run(int *OutChecks, int *OutFailures)
{
	Checks = 0;
	Failures = 0;

	printf("needle - shape, bounds and ring clearance\n");
	TestShape();
	printf("needle - symmetry and sweep ends\n");
	TestSymmetry();
	TestEnds();
	printf("needle - change detection, clipping, union\n");
	TestSame();
	TestClipping();
	TestUnion();

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
