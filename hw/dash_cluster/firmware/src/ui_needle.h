/*
 * ui_needle.h
 *
 * The gauge needle: where it is, what it covers, and drawing it into an 8-bit
 * buffer. No hardware and no display dependency, so it is host-tested.
 *
 * HARD-EDGED, ON PURPOSE. At 266 dpi one pixel subtends under half an arcminute
 * at a dashboard's viewing distance, so the needle is drawn without
 * anti-aliasing: a pixel is the needle's if its centre lies within half the
 * stroke of the needle's centre line. That is a round-ended stroke - the same
 * shape LVGL drew - and it is one palette index, with no blending.
 *
 * COORDINATES ARE CONTINUOUS. Pixel (x, y) covers [x, x+1) x [y, y+1), so its
 * centre is (x + 0.5, y + 0.5). The pivot of a 447 px dial at 9,9 is therefore
 * 232.5, 232.5 - the true centre of the face the renderer drew, rather than a
 * pixel beside it.
 */
#ifndef UI_NEEDLE_H_
#define UI_NEEDLE_H_

#include <stdbool.h>
#include <stdint.h>

/* An inclusive pixel rectangle. */
typedef struct
{
	int32_t X1;
	int32_t Y1;
	int32_t X2;
	int32_t Y2;
} UiRect_t;

typedef struct
{
	float X0, Y0;		/* the inner end of the centre line */
	float X1, Y1;		/* the tip */
	float HalfWidth;	/* also the radius of the rounded ends */
} UiNeedle_t;

/* Put a needle at a position: PositionQ is 0..UI_POSITION_MAX with UI_NEEDLE_Q
   fractional bits, as UiModel_NeedleStep() produces, turned into an angle the
   same way the face's graduations were laid out (ui_gauge.h). Cx, Cy is the
   pivot; Inner and Outer are distances from it along the needle. */
extern UiNeedle_t UiNeedle_Place(float Cx, float Cy, float Inner, float Outer,
                                 float HalfWidth, uint32_t PositionQ);

/* Every pixel the needle can touch. Exact to the pixel centres, so at most a
   row or column of it can come out empty - this is what is sent each frame. */
extern UiRect_t UiNeedle_Bounds(const UiNeedle_t *Needle);

/* True if two needles would set exactly the same pixels, judged from their
   geometry: the ends agree to 1/64 px. Cheaper than drawing both, and good
   enough to skip a frame on - a smaller movement cannot change a pixel centre
   test anywhere but on a knife edge. */
extern bool UiNeedle_Same(const UiNeedle_t *A, const UiNeedle_t *B);

/* Draw the needle into an 8-bit buffer Width x Height, Stride bytes a row,
   clipped to it. Returns the number of pixels set. */
extern uint32_t UiNeedle_Draw(const UiNeedle_t *Needle, uint8_t *Buffer,
                              uint32_t Stride, int32_t Width, int32_t Height,
                              uint8_t Index);

/* The smallest rectangle holding both. */
extern UiRect_t UiRect_Union(const UiRect_t *A, const UiRect_t *B);

#endif /* UI_NEEDLE_H_ */
