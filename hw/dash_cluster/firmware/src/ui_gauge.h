/*
 * ui_gauge.h
 *
 * The needle gauge's geometry and dial, shared by the firmware and the face
 * renderer.
 *
 * A gauge's graduations and numbers are not drawn live any more. They are
 * rendered once on the PC by tools/face_render - with this same LVGL checkout,
 * this same lv_conf.h and this same UiGauge_CreateScale() - and embedded in the
 * firmware as an image (src/dash_faces.c). Measured on the board, lv_scale
 * spent about two thirds of every frame regenerating ticks and labels outside
 * the area being redrawn; a picture costs a copy.
 *
 * The needle is still drawn live, in ui_lvgl.c, and it has to land on those
 * pre-rendered graduations: the same start angle, the same sweep, the same
 * placement to the pixel. So both sides take all of it from here rather than
 * each keeping a copy that could drift.
 */
#ifndef UI_GAUGE_H_
#define UI_GAUGE_H_

#include <stdint.h>

#include "lvgl.h"
#include "pages.h"

/* How far a gauge sweeps, and where it starts.
 *
 * 270 degrees beginning at 135 is the car-instrument convention: the needle
 * rests at the lower left, climbs over the top and finishes at the lower
 * right, leaving the bottom of the face clear for the signal's name. */
#define UI_GAUGE_ANGLE_RANGE		(270u)
#define UI_GAUGE_ROTATION		(135)

/* Minor ticks per major interval: four, so a rev counter marked in thousands
   gets a tick every 250 - with a medium one at every 500, as the MR2 has. The
   label list decides how many majors there are, so the total follows. */
#define UI_GAUGE_MINOR_PER_MAJOR	(4u)

/* The graduations, in pixels, after the SW20 MR2 tachometer: three weights of
   tick, all reaching the rim. Numerals sit inside them, pulled in far enough
   that the widest - "250" at three o'clock, where a label's whole width lies
   along the radius - clears the ends of the major ticks. */
#define UI_GAUGE_MAJOR_LENGTH		(24)
#define UI_GAUGE_MAJOR_WIDTH		(6)
#define UI_GAUGE_HALF_LENGTH		(16)
#define UI_GAUGE_HALF_WIDTH		(4)
#define UI_GAUGE_MINOR_LENGTH		(10)
#define UI_GAUGE_MINOR_WIDTH		(3)
#define UI_GAUGE_NUMERAL_PAD		(14)

/* The legend's centre, below the dial's centre, as a percent of the radius:
   under the centre ring and above the gap between the lowest two numerals. */
#define UI_GAUGE_LEGEND_PCT		(66)

/* Colours, 0xRRGGBB, taken from the MR2's lit instruments: a charcoal face,
   warm-white markings, a red warning band and a red needle. The needle is the
   brighter red, so it stays visible where it crosses the band. */
#define UI_GAUGE_FACE_COLOUR		(0x242221)
#define UI_GAUGE_MARK_COLOUR		(0xF2EEE4)
#define UI_GAUGE_BAND_COLOUR		(0xC8201A)
#define UI_GAUGE_NEEDLE_COLOUR		(0xFF3B30)
#define UI_GAUGE_RING_COLOUR		(0x6E6A64)

/* Where the needle starts, as a percent of the dial's radius: the middle of
   the dial is left empty, the way many real instruments leave a hub. 50% of
   the radius and a disc 50% of the diameter are the same circle. */
#define UI_GAUGE_NEEDLE_INNER_PCT	(50)

/* The needle's stroke. Its ends are rounded, so each end reaches half this
   past the point it is drawn to. */
#define UI_GAUGE_NEEDLE_WIDTH		(5)

/* The ring framing the value in the middle of the dial. Part of the dial, so
   it is baked into the pre-rendered face and costs nothing per frame. */
#define UI_GAUGE_RING_WIDTH		(3)

/* The dial's radius, as both the needle and the ring measure it. */
static inline int32_t UiGauge_Radius(int32_t W, int32_t H)
{
	return (W < H ? W : H) / 2;
}

/* Pixels from the pivot to where the needle is drawn from. */
static inline int32_t UiGauge_NeedleInner(int32_t W, int32_t H)
{
	return (UiGauge_Radius(W, H) * UI_GAUGE_NEEDLE_INNER_PCT) / 100;
}

/* The ring's OUTER radius: exactly where the needle's rounded inner end stops,
   so the needle looks as if it comes out from underneath the ring.

   It cannot actually pass underneath - the ring is in the face image and the
   needle is drawn live on top of it - so the illusion depends on the two
   meeting, not overlapping. Derived from the needle rather than set on its
   own, so moving the needle's start moves the ring with it. The ring's inner
   edge is then about 106 px from the centre, well clear of the widest value,
   "230.0", whose corners reach about 78 px. */
static inline int32_t UiGauge_RingRadius(int32_t W, int32_t H)
{
	return UiGauge_NeedleInner(W, H) - (UI_GAUGE_NEEDLE_WIDTH / 2);
}

/* Element geometry is in percent of the panel. */
static inline int32_t UiGauge_Pct(uint8_t Percent, int32_t Extent)
{
	return (int32_t)(((int32_t)Percent * Extent) / 100);
}

/* The dial itself - graduations and numbers - as an lv_scale in the NORMAL
   state's colours, at X/Y/W/H in the parent's coordinates.

   The face renderer snapshots exactly this to make the embedded image. The
   firmware only calls it when a gauge has no image, or its image no longer
   fits the element, so the face is still drawn - just slowly - rather than
   missing. */
extern lv_obj_t *UiGauge_CreateScale(lv_obj_t *Parent, const FaceElement_t *Element,
                                     int32_t X, int32_t Y, int32_t W, int32_t H);

#endif /* UI_GAUGE_H_ */
