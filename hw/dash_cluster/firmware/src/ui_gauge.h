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

/* SWEEPS. Angles are degrees, 0 at three o'clock and increasing CLOCKWISE on
 * the screen (y grows downward), which is lv_scale's convention too. A gauge
 * runs from its start angle through its span; a negative span runs
 * anticlockwise, which is how the bottom half reads left to right.
 *
 * The two halves stop HALF_MARGIN short of the horizontal. Their needles then
 * can never lie on top of each other - boost at full scale and a lean AFR both
 * point right - and, the reason for the size, the numerals at the two ends
 * clear each other: at 5 degrees "-1" sat on "10" and "1.5" on "20". 12 puts
 * about 60 px between each pair. */
#define UI_GAUGE_HALF_MARGIN_DEG	(12)

static inline int32_t UiGauge_SweepStart(GaugeSweep_t Sweep)
{
	switch (Sweep)
	{
	case GAUGE_SWEEP_TOP:		return 180 + UI_GAUGE_HALF_MARGIN_DEG;
	case GAUGE_SWEEP_BOTTOM:	return 180 - UI_GAUGE_HALF_MARGIN_DEG;
	case GAUGE_SWEEP_CLOCK:		return -90;	/* twelve at the top */
	default:			return UI_GAUGE_ROTATION;
	}
}

static inline int32_t UiGauge_SweepSpan(GaugeSweep_t Sweep)
{
	switch (Sweep)
	{
	case GAUGE_SWEEP_TOP:		return 180 - (2 * UI_GAUGE_HALF_MARGIN_DEG);
	case GAUGE_SWEEP_BOTTOM:	return -(180 - (2 * UI_GAUGE_HALF_MARGIN_DEG));
	case GAUGE_SWEEP_CLOCK:		return 360;
	default:			return (int32_t)UI_GAUGE_ANGLE_RANGE;
	}
}

/* Minor ticks between one numeral and the next: five on a clock, whose
   numerals are hours and whose minor ticks are minutes. */
static inline uint32_t UiGauge_MinorPerMajor(GaugeSweep_t Sweep)
{
	return (Sweep == GAUGE_SWEEP_CLOCK) ? 5u : UI_GAUGE_MINOR_PER_MAJOR;
}

/* HOW MANY TICKS, AND OVER WHAT ANGLE, the face's tick rings are drawn with.
 *
 * An open sweep has a tick at both ends - nine numerals on the rev counter
 * means eight intervals - so its count is one more than its intervals, over
 * its whole span. A clock closes on itself: its last tick must not land on
 * its first, so it gets one tick per interval over a span one interval short
 * of the full circle. The hands still use the full 360 from
 * UiGauge_SweepSpan(), which is what puts the second hand exactly on the tick
 * it is pointing at. */
static inline uint32_t UiGauge_TickCount(GaugeSweep_t Sweep, uint32_t Majors)
{
	if (Sweep == GAUGE_SWEEP_CLOCK)
		return Majors * UiGauge_MinorPerMajor(Sweep);
	return ((Majors - 1u) * UiGauge_MinorPerMajor(Sweep)) + 1u;
}

static inline int32_t UiGauge_TickSpan(GaugeSweep_t Sweep, uint32_t Majors)
{
	if (Sweep == GAUGE_SWEEP_CLOCK)
	{
		uint32_t Ticks = UiGauge_TickCount(Sweep, Majors);

		return 360 - (int32_t)(360u / Ticks);
	}
	return UiGauge_SweepSpan(Sweep);
}

/* A SPLIT FACE puts each half's reading inside the centre ring on its own side
   of a divider, with its legend beyond it: READING_DY is the reading's centre
   and LEGEND_DY the legend's, in pixels from the dial's centre. A full gauge
   has its reading on the centre and its legend below, at LEGEND_PCT of the
   radius. */
#define UI_GAUGE_SPLIT_READING_DY	(30)
#define UI_GAUGE_SPLIT_LEGEND_DY	(72)

/* The divider across the ring's interior on a split face: its thickness, and
   how far short of the ring it stops at each end. */
#define UI_GAUGE_DIVIDER_WIDTH		(2)
#define UI_GAUGE_DIVIDER_INSET		(14)

static inline int32_t UiGauge_ReadingDy(GaugeSweep_t Sweep)
{
	switch (Sweep)
	{
	case GAUGE_SWEEP_TOP:		return -UI_GAUGE_SPLIT_READING_DY;
	case GAUGE_SWEEP_BOTTOM:	return UI_GAUGE_SPLIT_READING_DY;
	default:			return 0;
	}
}

static inline int32_t UiGauge_LegendDy(GaugeSweep_t Sweep, int32_t Radius)
{
	switch (Sweep)
	{
	case GAUGE_SWEEP_TOP:		return -UI_GAUGE_SPLIT_LEGEND_DY;
	case GAUGE_SWEEP_BOTTOM:	return UI_GAUGE_SPLIT_LEGEND_DY;
	default:			return (Radius * UI_GAUGE_LEGEND_PCT) / 100;
	}
}

/* THE G-FORCE FACE. A friction circle: rings at 0.5, 1.0 and 1.5 g round the
   dial's centre, crosshairs, and direction labels in the band between the
   outer two rings. Pixels per g sets the scale - 1.5 g lands on the outer ring.
   The two readings sit outside the rings, longitudinal above and lateral
   below, where the face is plain. */
#define UI_GMETER_FULL_SCALE_MG		(1500)
#define UI_GMETER_PX_PER_G		(105)	/* 1.5 g at 157 px, up to the readings */
#define UI_GMETER_RING_WIDTH		(2)
#define UI_GMETER_CROSS_WIDTH		(1)
#define UI_GMETER_LABEL_R		(131)	/* between the 1.0 and 1.5 g rings */
#define UI_GMETER_READING_DY		(190)
#define UI_GMETER_DOT_R			(7)
#define UI_GMETER_TRAIL_R		(2)
#define UI_GMETER_PEAK_HALF		(6)	/* half the length of a peak mark */

/* THE STRIP CHART. A plot rectangle inside the round face, its frame drawn by
   the face renderer and its interior by the firmware. Offsets are from the
   dial's centre; the interior is the frame less its border, which comes to
   UI_GRAPH_COLUMNS wide. Readings sit above the plot with their legends over
   them, and the time axis below it - all clear of the plot and inside the
   disc at their widest. */
#define UI_GRAPH_FRAME_W		(304)
#define UI_GRAPH_FRAME_H		(194)
#define UI_GRAPH_FRAME_WIDTH		(2)
#define UI_GRAPH_READING_DX		(76)
#define UI_GRAPH_READING_DY		(-142)
#define UI_GRAPH_LEGEND_DY		(-178)
#define UI_GRAPH_TIME_DY		(121)
#define UI_GRAPH_AXIS_GAP		(8)	/* plot edge to its scale's labels */
#define UI_GRAPH_AXIS_W			(80)	/* the box a scale's labels align in */

/* THE CLOCK'S HANDS, as percentages of the dial's radius and pixels of stroke.
   Each reaches a little behind the pivot, as a real clock's hands do - it is
   what stops them looking like needles. The hub is drawn over them. */
#define UI_CLOCK_HOUR_PCT		(52)
#define UI_CLOCK_MINUTE_PCT		(80)
#define UI_CLOCK_SECOND_PCT		(86)
#define UI_CLOCK_HOUR_WIDTH		(8)
#define UI_CLOCK_MINUTE_WIDTH		(5)
#define UI_CLOCK_SECOND_WIDTH		(2)
#define UI_CLOCK_HOUR_TAIL		(16)
#define UI_CLOCK_MINUTE_TAIL		(20)
#define UI_CLOCK_SECOND_TAIL		(28)
#define UI_CLOCK_HUB_R			(7)

/* The digital time, in the space between the centre and the six - roughly
   where a watch puts its date window.

   This used to sit at 196, chosen to be outside the minute hand's 80% reach so
   the hands could never cross it. That put it hard against the numerals and
   the tick ring at the bottom of the dial, which is where it looked wrong. So
   it moved inboard instead and the crossing is handled: UiClockPage_Update
   redraws the reading whenever a hand has been over it, which keeps the text
   on top and intact. */
#define UI_CLOCK_DIGITAL_DY		(118)

/* THE DIGITAL CLOCK VIEW: hours and minutes in 160 px figures, seconds in the
   analogue face's 36 px figures under them. Offsets are of each line's centre
   from the dial's, chosen so the two together sit centred: the big figures
   are 114 px tall, a 24 px gap, then 26 px of seconds. */
#define UI_CLOCK_BIG_DY			(-25)
#define UI_CLOCK_SECONDS_DY		(70)

/* Element geometry is in percent of the panel. */
static inline int32_t UiGauge_Pct(uint8_t Percent, int32_t Extent)
{
	return (int32_t)(((int32_t)Percent * Extent) / 100);
}

#endif /* UI_GAUGE_H_ */
