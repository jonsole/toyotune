/*
 * ui_gpage.c - the g-force page. See ui_gpage.h.
 */

#include "ui_gpage.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "dash_font.h"
#include "imu.h"
#include "ui_draw.h"
#include "ui_gauge.h"
#include "ui_text.h"

extern const DashFont_t dash_font_value_56;

/* Where the dot stops: on the outer ring. With the rings reaching out towards
   the readings, anything further would run into them - the dot's edge is then
   about 5 px short of the nearer reading. */
#define UI_GPAGE_CLAMP_G	(1.5f)

/* Peaks this small are not worth a mark. */
#define UI_GPAGE_PEAK_MIN_G	(0.05f)

static GMeter_t		Model;
static GMeterCal_t	Cal;


/***************************************************************************************/
void UiGPage_Init(void)
{
	GMeter_Init(&Model);
	GMeter_DefaultCal(&Cal);
}


/***************************************************************************************/
void UiGPage_Sample(uint32_t FrameUs)
{
	ImuMilliG_t A;
	GMeterMg_t Raw;

	if (!Imu_Read(&A))
		return;

	Raw.X = A.X;
	Raw.Y = A.Y;
	Raw.Z = A.Z;
	GMeter_Update(&Model, &Cal, &Raw, FrameUs);
}


/***************************************************************************************/
void UiGPage_ResetPeaks(void)
{
	GMeter_ResetPeaks(&Model);
}

bool UiGPage_Zero(void)
{
	bool Ok = GMeter_Zero(&Model, &Cal);

	if (Ok)
	{
		GMeter_ResetPeaks(&Model);
		Model.TrailCount = 0u;
	}
	return Ok;
}

const GMeter_t *UiGPage_Model(void)		{ return &Model; }
const GMeterCal_t *UiGPage_Calibration(void)	{ return &Cal; }


/***************************************************************************************/
/* A dot at a g position, clamped to the plot. */
static UiNeedle_t UiGPage_Dot(const UiGPage_t *P, float Lat, float Lon, float Radius)
{
	UiNeedle_t N;
	float Mag = sqrtf((Lat * Lat) + (Lon * Lon));

	if (Mag > UI_GPAGE_CLAMP_G)
	{
		Lat *= UI_GPAGE_CLAMP_G / Mag;
		Lon *= UI_GPAGE_CLAMP_G / Mag;
	}

	N.X0 = P->Cx + (Lat * (float)UI_GMETER_PX_PER_G);
	N.Y0 = P->Cy - (Lon * (float)UI_GMETER_PX_PER_G);
	N.X1 = N.X0;
	N.Y1 = N.Y0;
	N.HalfWidth = Radius;
	return N;
}

/* A short mark across an axis at a peak. Horizontal axis: a vertical mark. */
static UiNeedle_t UiGPage_Peak(const UiGPage_t *P, float Lat, float Lon)
{
	UiNeedle_t N = UiGPage_Dot(P, Lat, Lon, 1.5f);

	if (Lon == 0.0f)
	{
		N.Y0 -= (float)UI_GMETER_PEAK_HALF;
		N.Y1 += (float)UI_GMETER_PEAK_HALF;
	}
	else
	{
		N.X0 -= (float)UI_GMETER_PEAK_HALF;
		N.X1 += (float)UI_GMETER_PEAK_HALF;
	}
	return N;
}


/***************************************************************************************/
/* Everything the page shows now, back to front: peaks, the trail oldest first
   and fading, then the dot. */
static uint32_t UiGPage_Shapes(const UiGPage_t *P, UiNeedle_t *Shapes, uint8_t *Levels)
{
	uint32_t n = 0, i;

	if (Model.PeakRight >= UI_GPAGE_PEAK_MIN_G)
	{
		Shapes[n] = UiGPage_Peak(P, Model.PeakRight, 0.0f);
		Levels[n++] = UI_DRAW_RED_FULL;
	}
	if (Model.PeakLeft >= UI_GPAGE_PEAK_MIN_G)
	{
		Shapes[n] = UiGPage_Peak(P, -Model.PeakLeft, 0.0f);
		Levels[n++] = UI_DRAW_RED_FULL;
	}
	if (Model.PeakAccel >= UI_GPAGE_PEAK_MIN_G)
	{
		Shapes[n] = UiGPage_Peak(P, 0.0f, Model.PeakAccel);
		Levels[n++] = UI_DRAW_RED_FULL;
	}
	if (Model.PeakBrake >= UI_GPAGE_PEAK_MIN_G)
	{
		Shapes[n] = UiGPage_Peak(P, 0.0f, -Model.PeakBrake);
		Levels[n++] = UI_DRAW_RED_FULL;
	}

	for (i = 0; i < Model.TrailCount; i++)
	{
		Shapes[n] = UiGPage_Dot(P, Model.TrailLat[i], Model.TrailLon[i],
		                        (float)UI_GMETER_TRAIL_R);
		/* Oldest faintest, from the faintest shade there is, so the tail
		   dissolves into the face rather than ending in a visible dot. */
		Levels[n++] = (uint8_t)(1u + ((i + 1u) * 11u) / GMETER_TRAIL_POINTS);
	}

	Shapes[n] = UiGPage_Dot(P, Model.Lat, Model.Lon, (float)UI_GMETER_DOT_R);
	Levels[n++] = UI_DRAW_RED_FULL;
	return n;
}


/***************************************************************************************/
/* A reading in hundredths of a g, signed: "0.42", "-0.64". */
static void UiGPage_Format(float G, char *Out, size_t Size)
{
	long Centi = lroundf(G * 100.0f);
	long Mag;

	/* The sensor tops out at 4 g; anything past 9.99 is not a reading. */
	if (Centi > 999)
		Centi = 999;
	if (Centi < -999)
		Centi = -999;
	Mag = (Centi < 0) ? -Centi : Centi;

	(void)snprintf(Out, Size, "%s%ld.%02ld", (Centi < 0) ? "-" : "", Mag / 100, Mag % 100);
}


/***************************************************************************************/
static void UiGPage_Readings(UiGPage_t *P, bool Force, UiGPageDirty_t Dirty, void *Ctx)
{
	float Values[2];
	float Dy[2];
	int k;

	Values[0] = Model.Lon;
	Values[1] = Model.Lat;
	Dy[0] = -(float)UI_GMETER_READING_DY;
	Dy[1] = (float)UI_GMETER_READING_DY;

	for (k = 0; k < 2; k++)
	{
		char Text[sizeof(P->Text[0])];
		int32_t Tx, Ty;
		UiRect_t Ink;

		UiGPage_Format(Values[k], Text, sizeof(Text));
		if (!Force && P->HaveText[k] && strcmp(Text, P->Text[k]) == 0)
			continue;

		if (P->HaveText[k])
		{
			UiDraw_Restore(P->Surface, &P->TextRect[k]);
			if (Dirty != NULL)
				Dirty(Ctx, &P->TextRect[k]);
		}

		memcpy(P->Text[k], Text, sizeof(Text));
		UiText_Centre(&dash_font_value_56, Text, P->Cx, P->Cy + Dy[k], &Tx, &Ty);
		P->HaveText[k] = UiText_Bounds(&dash_font_value_56, Text, Tx, Ty, &Ink);
		if (P->HaveText[k])
		{
			(void)UiDraw_Text(P->Surface, &dash_font_value_56, Text, Tx, Ty);
			P->TextRect[k] = Ink;
			if (Dirty != NULL)
				Dirty(Ctx, &Ink);
		}
	}
}


/***************************************************************************************/
void UiGPage_Load(UiGPage_t *P, uint8_t Surface, float Cx, float Cy)
{
	uint32_t i;

	memset(P, 0, sizeof(*P));
	P->Surface = Surface;
	P->Cx = Cx;
	P->Cy = Cy;

	P->ShapeCount = UiGPage_Shapes(P, P->Shapes, P->Levels);
	for (i = 0; i < P->ShapeCount; i++)
		(void)UiDraw_Stroke(Surface, &P->Shapes[i], P->Levels[i]);

	UiGPage_Readings(P, true, NULL, NULL);
}


/***************************************************************************************/
/* The shapes overlap each other - the trail runs under the dot, the dot over a
   peak mark - so when anything has moved, ALL of the old ones are put back
   before ANY of the new ones are drawn. They never reach the readings, which
   sit outside the clamp, so those are handled on their own. */
void UiGPage_Update(UiGPage_t *P, bool TextDue, UiGPageDirty_t Dirty, void *Ctx)
{
	UiNeedle_t Shapes[UI_GPAGE_SHAPES];
	uint8_t Levels[UI_GPAGE_SHAPES];
	uint32_t n = UiGPage_Shapes(P, Shapes, Levels);
	bool Same = (n == P->ShapeCount);
	uint32_t i;

	for (i = 0; Same && i < n; i++)
		Same = (Levels[i] == P->Levels[i]) && UiNeedle_Same(&Shapes[i], &P->Shapes[i]);

	if (!Same)
	{
		for (i = 0; i < P->ShapeCount; i++)
		{
			UiRect_t R = UiNeedle_Bounds(&P->Shapes[i]);

			UiDraw_Restore(P->Surface, &R);
			Dirty(Ctx, &R);
		}
		for (i = 0; i < n; i++)
		{
			UiRect_t R = UiNeedle_Bounds(&Shapes[i]);

			(void)UiDraw_Stroke(P->Surface, &Shapes[i], Levels[i]);
			Dirty(Ctx, &R);
		}
		memcpy(P->Shapes, Shapes, sizeof(Shapes[0]) * n);
		memcpy(P->Levels, Levels, sizeof(Levels[0]) * n);
		P->ShapeCount = n;
	}

	if (TextDue)
		UiGPage_Readings(P, false, Dirty, Ctx);
}
