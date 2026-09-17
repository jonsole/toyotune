/*
 * ui_needle.c - the gauge needle. See ui_needle.h.
 */

#include "ui_needle.h"

#include <math.h>

#include "ui_gauge.h"
#include "ui_model.h"

#define UI_NEEDLE_DEG_TO_RAD	(3.14159265358979f / 180.0f)


/***************************************************************************************/
UiNeedle_t UiNeedle_Place(float Cx, float Cy, float Inner, float Outer,
                          float HalfWidth, uint32_t PositionQ)
{
	UiNeedle_t N;

	/* The graduations were laid out by lv_scale over UI_GAUGE_ANGLE_RANGE
	   degrees from UI_GAUGE_ROTATION, 0 at three o'clock and clockwise - which,
	   with y growing downward, is plain cos and sin. */
	float Deg = (float)UI_GAUGE_ROTATION
	            + ((float)UI_GAUGE_ANGLE_RANGE * (float)PositionQ)
	              / ((float)UI_POSITION_MAX * (float)(1u << UI_NEEDLE_Q));
	float C = cosf(Deg * UI_NEEDLE_DEG_TO_RAD);
	float S = sinf(Deg * UI_NEEDLE_DEG_TO_RAD);

	N.X0 = Cx + (Inner * C);
	N.Y0 = Cy + (Inner * S);
	N.X1 = Cx + (Outer * C);
	N.Y1 = Cy + (Outer * S);
	N.HalfWidth = HalfWidth;
	return N;
}


/***************************************************************************************/
UiRect_t UiNeedle_Bounds(const UiNeedle_t *N)
{
	UiRect_t R;
	float MinX = (N->X0 < N->X1) ? N->X0 : N->X1;
	float MaxX = (N->X0 > N->X1) ? N->X0 : N->X1;
	float MinY = (N->Y0 < N->Y1) ? N->Y0 : N->Y1;
	float MaxY = (N->Y0 > N->Y1) ? N->Y0 : N->Y1;

	/* Every pixel whose centre can be within HalfWidth of the line: its centre
	   x + 0.5 lies in [Min - H, Max + H]. Tight, because this rectangle is what
	   goes to the panel; and safe whatever the rounding, because the draw loop
	   only ever visits pixels inside it. */
	R.X1 = (int32_t)ceilf(MinX - N->HalfWidth - 0.5f);
	R.Y1 = (int32_t)ceilf(MinY - N->HalfWidth - 0.5f);
	R.X2 = (int32_t)floorf(MaxX + N->HalfWidth - 0.5f);
	R.Y2 = (int32_t)floorf(MaxY + N->HalfWidth - 0.5f);
	return R;
}


/***************************************************************************************/
static int32_t UiNeedle_Q6(float V)
{
	return (int32_t)lroundf(V * 64.0f);
}

bool UiNeedle_Same(const UiNeedle_t *A, const UiNeedle_t *B)
{
	return UiNeedle_Q6(A->X0) == UiNeedle_Q6(B->X0)
	       && UiNeedle_Q6(A->Y0) == UiNeedle_Q6(B->Y0)
	       && UiNeedle_Q6(A->X1) == UiNeedle_Q6(B->X1)
	       && UiNeedle_Q6(A->Y1) == UiNeedle_Q6(B->Y1)
	       && UiNeedle_Q6(A->HalfWidth) == UiNeedle_Q6(B->HalfWidth);
}


/***************************************************************************************/
/* Per pixel over the bounding box: distance from the pixel centre to the
 * centre line, clamped to its ends, against the half-width. The needle's box is
 * at most about 90 px square, so this is some thousands of multiply-adds a
 * frame on a core with an FPU - well under a millisecond, and plainly correct,
 * which a span-walking version would have to be shown to be.
 *
 * Each row's hits are contiguous, since the shape is convex, so the inner loop
 * stops at the first miss after a hit. */
uint32_t UiNeedle_Draw(const UiNeedle_t *N, uint8_t *Buffer, uint32_t Stride,
                       int32_t Width, int32_t Height, uint8_t Index)
{
	UiRect_t R = UiNeedle_Bounds(N);
	float Dx = N->X1 - N->X0;
	float Dy = N->Y1 - N->Y0;
	float Len2 = (Dx * Dx) + (Dy * Dy);
	float InvLen2 = (Len2 > 0.0f) ? (1.0f / Len2) : 0.0f;
	float R2 = N->HalfWidth * N->HalfWidth;
	uint32_t Set = 0;
	int32_t X, Y;

	if (R.X1 < 0) R.X1 = 0;
	if (R.Y1 < 0) R.Y1 = 0;
	if (R.X2 > Width - 1) R.X2 = Width - 1;
	if (R.Y2 > Height - 1) R.Y2 = Height - 1;

	for (Y = R.Y1; Y <= R.Y2; Y++)
	{
		float Py = ((float)Y + 0.5f) - N->Y0;
		uint8_t *Row = Buffer + ((uint32_t)Y * Stride);
		bool Hit = false;

		for (X = R.X1; X <= R.X2; X++)
		{
			float Px = ((float)X + 0.5f) - N->X0;
			float T = ((Px * Dx) + (Py * Dy)) * InvLen2;
			float Ex, Ey;

			if (T < 0.0f)
				T = 0.0f;
			else if (T > 1.0f)
				T = 1.0f;

			Ex = Px - (T * Dx);
			Ey = Py - (T * Dy);

			if (((Ex * Ex) + (Ey * Ey)) <= R2)
			{
				Row[X] = Index;
				Set++;
				Hit = true;
			}
			else if (Hit)
			{
				break;
			}
		}
	}

	return Set;
}


/***************************************************************************************/
UiRect_t UiRect_Union(const UiRect_t *A, const UiRect_t *B)
{
	UiRect_t R;

	R.X1 = (A->X1 < B->X1) ? A->X1 : B->X1;
	R.Y1 = (A->Y1 < B->Y1) ? A->Y1 : B->Y1;
	R.X2 = (A->X2 > B->X2) ? A->X2 : B->X2;
	R.Y2 = (A->Y2 > B->Y2) ? A->Y2 : B->Y2;
	return R;
}
