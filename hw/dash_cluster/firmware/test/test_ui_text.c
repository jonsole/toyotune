/*
 * test_ui_text.c - host tests for text drawn into the paletted buffer, using
 * the firmware's own generated value font.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dash_font.h"
#include "ui_text.h"

extern int TextTests_Run(int *Checks, int *Failures);
extern const DashFont_t dash_font_value_56;

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

#define W	(466)
#define H	(466)

static uint8_t Buf[W * H];

/* Ramp entries 0x81..0x8F, so a drawn pixel says what coverage it had. */
static const uint8_t Ramp[UI_TEXT_LEVELS] = {
	0x00, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
	0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F
};

#define F	(&dash_font_value_56)


/***************************************************************************************/
static void TestWidth(void)
{
	int32_t Cell = UiText_Width(F, "0");

	CHECK(Cell > 0, "a digit should advance");
	CHECK(UiText_Width(F, "8000") == 4 * Cell, "digits are tabular: 4 x %d, got %d",
	      Cell, UiText_Width(F, "8000"));
	CHECK(UiText_Width(F, "1111") == UiText_Width(F, "8888"),
	      "a 1 must be as wide as an 8, or a changing reading shuffles");
	CHECK(UiText_Width(F, "8~8") == UiText_Width(F, "88"),
	      "a character the font lacks takes no space");
	CHECK(UiText_Width(F, "") == 0, "empty text has no width");
}


/***************************************************************************************/
/* Every pixel drawn lies inside the reported bounds, uses a ramp entry, and the
   count matches the glyph's own non-zero coverage - which is what shows the
   nibble walk across rows is right. */
static void TestDrawMatchesBitmap(void)
{
	const char *Samples[] = { "0", "8", "4", "-", "1234", "567.9", "--" };
	size_t n;

	for (n = 0; n < sizeof(Samples) / sizeof(Samples[0]); n++)
	{
		const char *T = Samples[n];
		UiRect_t R;
		uint32_t Set, Counted = 0, Outside = 0, Bad = 0, Expected = 0;
		int32_t X, Y;
		const char *c;

		memset(Buf, 0, sizeof(Buf));
		CHECK(UiText_Bounds(F, T, 100, 200, &R), "\"%s\" should have ink", T);
		Set = UiText_Draw(F, T, 100, 200, Ramp, Buf, W, W, H);

		for (Y = 0; Y < H; Y++)
			for (X = 0; X < W; X++)
			{
				uint8_t v = Buf[(Y * W) + X];

				if (v == 0)
					continue;
				Counted++;
				if (v < 0x81 || v > 0x8F)
					Bad++;
				if (X < R.X1 || X > R.X2 || Y < R.Y1 || Y > R.Y2)
					Outside++;
			}

		for (c = T; *c; c++)
		{
			const DashGlyph_t *G = &F->Glyphs[F->Map[*c - DASH_FONT_FIRST_CHAR] - 1];
			uint32_t i, Pixels = (uint32_t)G->Width * G->Height;

			for (i = 0; i < Pixels; i++)
			{
				uint8_t b = F->Bitmap[G->Bitmap + (i >> 1)];

				if (((i & 1u) ? (b & 0x0F) : (b >> 4)) != 0)
					Expected++;
			}
		}

		CHECK(Set == Counted, "\"%s\": reported %u, buffer has %u", T, Set, Counted);
		CHECK(Set == Expected, "\"%s\": drew %u, the glyphs hold %u", T, Set, Expected);
		CHECK(Bad == 0, "\"%s\": %u pixels not from the ramp", T, Bad);
		CHECK(Outside == 0, "\"%s\": %u pixels outside its bounds", T, Outside);
	}
}


/***************************************************************************************/
/* Centred on the dial's half-pixel centre, a reading's ink is centred there too. */
static void TestCentre(void)
{
	const char *Samples[] = { "8888", "808", "88" };
	size_t n;

	for (n = 0; n < sizeof(Samples) / sizeof(Samples[0]); n++)
	{
		int32_t X, Y, Px, Py;
		double Sx = 0, Sy = 0, Sw = 0;

		UiText_Centre(F, Samples[n], 232.5f, 232.5f, &X, &Y);
		memset(Buf, 0, sizeof(Buf));
		(void)UiText_Draw(F, Samples[n], X, Y, Ramp, Buf, W, W, H);

		for (Py = 0; Py < H; Py++)
			for (Px = 0; Px < W; Px++)
			{
				uint8_t v = Buf[(Py * W) + Px];

				if (v == 0)
					continue;
				Sx += (Px + 0.5) * (v - 0x80);
				Sy += (Py + 0.5) * (v - 0x80);
				Sw += (v - 0x80);
			}

		CHECK(fabs(Sx / Sw - 232.5) < 1.5 && fabs(Sy / Sw - 232.5) < 1.5,
		      "\"%s\" centred at %.1f,%.1f, wanted 232.5,232.5",
		      Samples[n], Sx / Sw, Sy / Sw);
	}
}


/***************************************************************************************/
static void TestClipping(void)
{
	enum { SW = 30, SH = 20, GUARD = 64 };
	static uint8_t Small[GUARD + (SW * SH) + GUARD];
	uint32_t i, Set;
	int Clean = 1;
	UiRect_t R;

	memset(Small, 0x11, sizeof(Small));
	Set = UiText_Draw(F, "8888", -10, 30, Ramp, Small + GUARD, SW, SW, SH);
	for (i = 0; i < GUARD; i++)
		if (Small[i] != 0x11 || Small[GUARD + (SW * SH) + i] != 0x11)
			Clean = 0;

	CHECK(Clean, "clipped text wrote outside its buffer");
	CHECK(Set > 0, "the visible part should still be drawn");
	CHECK(!UiText_Bounds(F, "", 0, 0, &R), "empty text has no ink");
	CHECK(!UiText_Bounds(F, "   ", 0, 0, &R), "spaces have no ink");
}


/***************************************************************************************/
int TextTests_Run(int *OutChecks, int *OutFailures)
{
	Checks = 0;
	Failures = 0;

	printf("text - widths, drawing, centring, clipping\n");
	TestWidth();
	TestDrawMatchesBitmap();
	TestCentre();
	TestClipping();

	*OutChecks += Checks;
	*OutFailures += Failures;
	return Failures;
}
