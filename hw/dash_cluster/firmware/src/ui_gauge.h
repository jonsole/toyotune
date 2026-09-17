/*
 * ui_gauge.h
 *
 * The needle gauge's geometry, shared by the firmware and the face renderer.
 *
 * A gauge's graduations and numbers are not drawn live. They are rendered once
 * on the PC by tools/face_render - which is the only place LVGL is still used
 * - and embedded here as an image (src/dash_faces.c).
 *
 * The needle IS drawn live, and it has to land on those pre-rendered
 * graduations: the same start angle, the same sweep, the same placement to the
 * pixel. So both sides take all of it from here rather than each keeping a
 * copy that could drift - which is why this header has no LVGL in it. The
 * dial-building half lives beside the renderer, in
 * tools/face_render/ui_gauge_scale.h.
 */
#ifndef UI_GAUGE_H_
#define UI_GAUGE_H_

#include <stdint.h>

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
   brighter red, so it stays visible where it crosses the band.

   The face was 0x242221 and read too light on the AMOLED, whose blacks are
   true black; it is now about two thirds of that, with the same warm cast. */
#define UI_GAUGE_FACE_COLOUR		(0x181716)
#define UI_GAUGE_MARK_COLOUR		(0xF2EEE4)
#define UI_GAUGE_BAND_COLOUR		(0xC8201A)
#define UI_GAUGE_NEEDLE_COLOUR		(0xFF3B30)
#define UI_GAUGE_RING_COLOUR		(0x6E6A64)

/* Where the needle starts, as a percent of the dial's radius: the middle of
   the dial is left empty, the way many real instruments leave a hub. 50% of
   the radius and a disc 50% of the diameter are the same circle. */
#define UI_GAUGE_NEEDLE_INNER_PCT	(50)

/* Where the needle ends: long enough to reach in among the graduations, short
   of the numerals' ring of the ticks' own inner ends. */
#define UI_GAUGE_NEEDLE_PCT		(85)

/* The needle's stroke. Its ends are rounded, so each end reaches half this
   past the point it is drawn to. */
#define UI_GAUGE_NEEDLE_WIDTH		(5)

/* The ring framing the value in the middle of the dial. Part of the dial, so
   it is baked into the pre-rendered face and costs nothing per frame. */
#define UI_GAUGE_RING_WIDTH		(3)

/* The warning band, broken into blocks by the ticks that cross it - the
   graduations show through the gaps and stand proud of the blocks, as on the
   car's own dial. INSET is how far inside the rim the band sits, so the ticks
   reach past it; GAP is the clear space, in pixels, either side of each tick.

   GAP is in PIXELS, and the same beside every tick, because the gaps are cut
   by the ticks themselves: see the face renderer's ui_gauge.c. It was once an
   angle given to lv_arc, which takes whole degrees while the ticks are 8.4375
   degrees apart - so each block's two ends rounded differently and the gaps
   either side of a tick differed by up to 4 px. */
#define UI_GAUGE_BAND_INSET		(3)
#define UI_GAUGE_BAND_WIDTH		(16)
#define UI_GAUGE_BAND_GAP		(2)

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

/* Pixels from the pivot to the needle's tip. */
static inline int32_t UiGauge_NeedleOuter(int32_t W, int32_t H)
{
	return (UiGauge_Radius(W, H) * UI_GAUGE_NEEDLE_PCT) / 100;
}

/* Element geometry is in percent of the panel. */
static inline int32_t UiGauge_Pct(uint8_t Percent, int32_t Extent)
{
	return (int32_t)(((int32_t)Percent * Extent) / 100);
}

#endif /* UI_GAUGE_H_ */
