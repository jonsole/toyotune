/*
 * ui_gauge.c
 *
 * The gauge dial, styled after the SW20 MR2's own instruments: a charcoal face,
 * heavy warm-white graduations every major with medium ones halfway and small
 * ones between, bold condensed numerals inside them, a red warning band blocked
 * between the ticks at the top of the range, and a legend.
 *
 * A face carries one full gauge, or two half gauges - top and bottom - sharing
 * its centre. The face itself (disc, centre ring, and the divider a split face
 * has) is drawn once by UiGauge_CreateFace(); each gauge then adds its own
 * graduations with UiGauge_CreateScale().
 *
 * Built only into the face renderer on the PC, which snapshots it into
 * src/dash_faces.c; the firmware does not link LVGL. Anything that changes how
 * the dial looks belongs here - and after changing it, rerun
 * tools/face_render/build_faces.py, or the firmware will keep showing the old
 * picture.
 *
 * Everything is drawn at UI_GAUGE_RENDER_SCALE times the panel's size, so
 * every pixel dimension from ui_gauge.h goes through S() on its way in. The
 * fonts are the same faces generated at four times the size, not scaled
 * bitmaps.
 */

#include <math.h>
#include <stdlib.h>

#include "ui_gauge_scale.h"
#include "ui_model.h"

LV_FONT_DECLARE(dash_font_numeral_160);
LV_FONT_DECLARE(dash_font_legend_104);

/* A panel-pixel dimension in render pixels. */
#define S(Px)		((Px) * UI_GAUGE_RENDER_SCALE)

/* lv_scale adds this to every label's radial padding - a private constant in
   lv_scale.c (LV_SCALE_DEFAULT_LABEL_GAP, 9.5.0), in pixels, and so NOT scaled
   with everything else. Left alone it put every numeral 11 panel pixels
   further out at 4x than at 1x, the "4" touching its tick. The padding set
   below is reduced by it so that the total gap scales like any other
   dimension. Check it if LVGL is updated. */
#define LV_SCALE_LABEL_GAP	(15)

/* The element's own units to the model's 0..UI_POSITION_MAX. */
static int32_t UiGauge_Position(const FaceElement_t *Element, int32_t Value)
{
	int32_t Span = Element->Max - Element->Min;

	if (Span <= 0 || Value <= Element->Min)
		return 0;
	if (Value >= Element->Max)
		return UI_POSITION_MAX;
	return ((Value - Element->Min) * UI_POSITION_MAX) / Span;
}

/* A position's angle on this gauge - the same arithmetic the firmware's needle
   uses, so the band and the needle agree. */
static double UiGauge_Angle(GaugeSweep_t Sweep, int32_t Position)
{
	return (double)UiGauge_SweepStart(Sweep)
	       + ((double)UiGauge_SweepSpan(Sweep) * (double)Position) / (double)UI_POSITION_MAX;
}

/* One ring of tick marks over the gauge's sweep. The graduations are two of
   these laid over each other, because lv_scale knows only "major" and "minor"
   and the MR2 has three weights of tick. Every theme style comes off first, so
   nothing - the rim line the theme draws, its colours, its lengths - leaks
   into the look.

   lv_scale only sweeps clockwise, so a gauge that reads anticlockwise - the
   bottom half, left to right - is drawn as the same arc clockwise from its
   other end, with its labels in the opposite order. The ticks are symmetric,
   so only the labels need to know. */
static lv_obj_t *UiGauge_TickRing(lv_obj_t *Parent, GaugeSweep_t Sweep,
                                  int32_t X, int32_t Y, int32_t W, int32_t H,
                                  uint32_t Total, uint32_t MajorEvery)
{
	lv_obj_t *Scale = lv_scale_create(Parent);
	int32_t Start = UiGauge_SweepStart(Sweep);
	int32_t Span = UiGauge_SweepSpan(Sweep);

	lv_obj_remove_style_all(Scale);
	lv_obj_set_pos(Scale, X, Y);
	lv_obj_set_size(Scale, W, H);
	lv_scale_set_mode(Scale, LV_SCALE_MODE_ROUND_INNER);
	if (Span >= 0)
	{
		lv_scale_set_angle_range(Scale, (uint32_t)Span);
		lv_scale_set_rotation(Scale, Start);
	}
	else
	{
		lv_scale_set_angle_range(Scale, (uint32_t)-Span);
		lv_scale_set_rotation(Scale, Start + Span);
	}

	/* The needle is driven with the model's normalised position, so the scale
	   counts in those units too and the printed numbers come from the label
	   list instead. One less place for a range to disagree. */
	lv_scale_set_range(Scale, 0, UI_POSITION_MAX);
	lv_scale_set_total_tick_count(Scale, Total);
	lv_scale_set_major_tick_every(Scale, MajorEvery);
	lv_scale_set_label_show(Scale, false);
	lv_obj_remove_flag(Scale, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	return Scale;
}

/* The label list in the order lv_scale will place it: as given for a
   clockwise gauge, reversed for an anticlockwise one. The scale keeps the
   pointer, so the reversed copy is allocated and never freed - this runs once
   per face in a program that exits straight afterwards. */
static const char **UiGauge_Labels(const FaceElement_t *Element, uint32_t Count)
{
	const char **Out;
	uint32_t i;

	if (UiGauge_SweepSpan(Element->Sweep) >= 0)
		return (const char **)Element->Ticks;

	Out = calloc(Count + 1u, sizeof(*Out));
	for (i = 0; i < Count; i++)
		Out[i] = Element->Ticks[Count - 1u - i];
	Out[Count] = NULL;
	return Out;
}


/***************************************************************************************/
lv_obj_t *UiGauge_CreateFace(lv_obj_t *Parent, int32_t X, int32_t Y,
                             int32_t W, int32_t H, bool Split)
{
	lv_obj_t *Face = lv_obj_create(Parent);
	int32_t PanelW = W / UI_GAUGE_RENDER_SCALE;
	int32_t PanelH = H / UI_GAUGE_RENDER_SCALE;

	/* A charcoal disc rather than bare black, as a lit instrument face is.
	   Costs the AMOLED a little light for a lot of look. */
	lv_obj_remove_style_all(Face);
	lv_obj_set_pos(Face, X, Y);
	lv_obj_set_size(Face, W, H);
	lv_obj_set_style_radius(Face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
	lv_obj_set_style_bg_color(Face, lv_color_hex(UI_GAUGE_FACE_COLOUR), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(Face, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_remove_flag(Face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

	/* The ring round the centre readout: a bordered circle with nothing
	   inside, its outer edge exactly where the needle's rounded inner end
	   stops - see UiGauge_RingRadius(). A border is drawn inside its object's
	   box, so a box of the ring's outer diameter puts the stroke's outer edge
	   on that radius. Computed from the PANEL-size geometry and then scaled:
	   the firmware places its needles from the panel size, and the two must
	   meet. */
	{
		int32_t RingD = 2 * S(UiGauge_RingRadius(PanelW, PanelH));
		lv_obj_t *Ring = lv_obj_create(Face);

		lv_obj_remove_style_all(Ring);
		lv_obj_set_size(Ring, RingD, RingD);
		lv_obj_align(Ring, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_radius(Ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(Ring, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(Ring, S(UI_GAUGE_RING_WIDTH), LV_PART_MAIN);
		lv_obj_set_style_border_color(Ring, lv_color_hex(UI_GAUGE_RING_COLOUR),
		                              LV_PART_MAIN);
		lv_obj_set_style_border_opa(Ring, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_remove_flag(Ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	}

	/* A split face divides the ring's interior between its two readings. */
	if (Split)
	{
		int32_t Inner = UiGauge_RingRadius(PanelW, PanelH) - UI_GAUGE_RING_WIDTH;
		int32_t Half = Inner - UI_GAUGE_DIVIDER_INSET;
		lv_obj_t *Divider = lv_obj_create(Face);

		lv_obj_remove_style_all(Divider);
		lv_obj_set_size(Divider, S(2 * Half), S(UI_GAUGE_DIVIDER_WIDTH));
		lv_obj_align(Divider, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_bg_color(Divider, lv_color_hex(UI_GAUGE_RING_COLOUR),
		                          LV_PART_MAIN);
		lv_obj_set_style_bg_opa(Divider, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_remove_flag(Divider, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	}

	return Face;
}


/***************************************************************************************/
lv_obj_t *UiGauge_CreateScale(lv_obj_t *Parent, const FaceElement_t *Element,
                              int32_t X, int32_t Y, int32_t W, int32_t H)
{
	lv_color_t Mark = lv_color_hex(UI_GAUGE_MARK_COLOUR);
	GaugeSweep_t Sweep = Element->Sweep;
	uint32_t Majors = 0;
	lv_obj_t *Scale;
	lv_obj_t *Half;

	if (Element->Ticks != NULL)
		while (Element->Ticks[Majors] != NULL)
			Majors++;
	if (Majors < 2u)
		Majors = 2u;	/* a scale with one tick is not a scale */

	/* Drawn in creation order, back to front: band, ticks, legend. The band
	   has to sit under the ticks, so it cannot be a child of the scale -
	   children draw after their parent. */

	/* The warning band: ONE arc from BandFrom to the end of the scale, inset
	   from the rim, which the ticks then cut into blocks - see the gap rings
	   below.

	   Why not a block per interval, as it used to be: lv_arc takes whole
	   degrees, the ticks are 8.4375 degrees apart on a full dial, and a block's
	   two ends rounded independently, so the gaps either side of a tick came
	   out different widths. A single arc has only two ends, and both are
	   hidden: each is rounded OUTWARD, onto the far side of its tick, where the
	   gap cut round that tick removes the overshoot - at most a degree, under
	   4 px at this radius, against a gap reaching over 5 px from a major
	   tick's centre.

	   Painted only: whether the gauge itself turns to its warning state is
	   UiModel_SignalWarning()'s decision, not this. */
	if (Element->BandFrom > Element->Min && Element->BandFrom < Element->Max)
	{
		double A = UiGauge_Angle(Sweep, UiGauge_Position(Element, Element->BandFrom));
		double B = UiGauge_Angle(Sweep, UI_POSITION_MAX);
		int32_t From = (int32_t)floor((A < B) ? A : B);
		int32_t To = (int32_t)ceil((A < B) ? B : A);
		lv_obj_t *Band = lv_arc_create(Parent);

		lv_obj_remove_style_all(Band);
		lv_obj_set_pos(Band, X + S(UI_GAUGE_BAND_INSET), Y + S(UI_GAUGE_BAND_INSET));
		lv_obj_set_size(Band, W - (2 * S(UI_GAUGE_BAND_INSET)),
		                H - (2 * S(UI_GAUGE_BAND_INSET)));
		lv_arc_set_bg_angles(Band, (From + 360) % 360, (To + 360) % 360);
		lv_obj_set_style_arc_width(Band, S(UI_GAUGE_BAND_WIDTH), LV_PART_MAIN);
		lv_obj_set_style_arc_color(Band, lv_color_hex(UI_GAUGE_BAND_COLOUR),
		                           LV_PART_MAIN);
		lv_obj_set_style_arc_opa(Band, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_arc_rounded(Band, false, LV_PART_MAIN);
		lv_obj_remove_flag(Band, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

		/* THE GAPS: the tick rings again, in the face's colour, each tick
		   2 x GAP wider than the white one that will be drawn over it and long
		   enough to cross the band. lv_scale places these exactly where it
		   places the white ticks - to a tenth of a degree, from the same counts
		   - so every gap is centred on its tick and the same width in pixels.
		   Everywhere else they are charcoal on charcoal, and invisible. They
		   cover only this gauge's own sweep, so on a split face they cannot
		   touch the other half's graduations. */
		{
			lv_color_t Face = lv_color_hex(UI_GAUGE_FACE_COLOUR);
			int32_t Reach = S(UI_GAUGE_BAND_INSET + UI_GAUGE_BAND_WIDTH + 1);
			lv_obj_t *Cut;

			Cut = UiGauge_TickRing(Parent, Sweep, X, Y, W, H,
			                       ((Majors - 1u) * UI_GAUGE_MINOR_PER_MAJOR) + 1u,
			                       UI_GAUGE_MINOR_PER_MAJOR);
			lv_obj_set_style_length(Cut, Reach, LV_PART_INDICATOR);
			lv_obj_set_style_line_width(Cut, S(UI_GAUGE_MAJOR_WIDTH + 2 * UI_GAUGE_BAND_GAP),
			                            LV_PART_INDICATOR);
			lv_obj_set_style_line_color(Cut, Face, LV_PART_INDICATOR);
			lv_obj_set_style_line_opa(Cut, LV_OPA_COVER, LV_PART_INDICATOR);
			lv_obj_set_style_length(Cut, Reach, LV_PART_ITEMS);
			lv_obj_set_style_line_width(Cut, S(UI_GAUGE_MINOR_WIDTH + 2 * UI_GAUGE_BAND_GAP),
			                            LV_PART_ITEMS);
			lv_obj_set_style_line_color(Cut, Face, LV_PART_ITEMS);
			lv_obj_set_style_line_opa(Cut, LV_OPA_COVER, LV_PART_ITEMS);

			/* The half-major ticks are a weight of their own. */
			Cut = UiGauge_TickRing(Parent, Sweep, X, Y, W, H, ((Majors - 1u) * 2u) + 1u, 2u);
			lv_obj_set_style_line_width(Cut, 0, LV_PART_INDICATOR);
			lv_obj_set_style_length(Cut, Reach, LV_PART_ITEMS);
			lv_obj_set_style_line_width(Cut, S(UI_GAUGE_HALF_WIDTH + 2 * UI_GAUGE_BAND_GAP),
			                            LV_PART_ITEMS);
			lv_obj_set_style_line_color(Cut, Face, LV_PART_ITEMS);
			lv_obj_set_style_line_opa(Cut, LV_OPA_COVER, LV_PART_ITEMS);
		}
	}

	/* Heavy ticks every major and small ones every minor, with the numerals. */
	Scale = UiGauge_TickRing(Parent, Sweep, X, Y, W, H,
	                         ((Majors - 1u) * UI_GAUGE_MINOR_PER_MAJOR) + 1u,
	                         UI_GAUGE_MINOR_PER_MAJOR);

	lv_obj_set_style_length(Scale, S(UI_GAUGE_MAJOR_LENGTH), LV_PART_INDICATOR);
	lv_obj_set_style_line_width(Scale, S(UI_GAUGE_MAJOR_WIDTH), LV_PART_INDICATOR);
	lv_obj_set_style_line_color(Scale, Mark, LV_PART_INDICATOR);
	lv_obj_set_style_line_opa(Scale, LV_OPA_COVER, LV_PART_INDICATOR);

	lv_obj_set_style_length(Scale, S(UI_GAUGE_MINOR_LENGTH), LV_PART_ITEMS);
	lv_obj_set_style_line_width(Scale, S(UI_GAUGE_MINOR_WIDTH), LV_PART_ITEMS);
	lv_obj_set_style_line_color(Scale, Mark, LV_PART_ITEMS);
	lv_obj_set_style_line_opa(Scale, LV_OPA_COVER, LV_PART_ITEMS);

	if (Element->Ticks != NULL)
	{
		lv_scale_set_text_src(Scale, UiGauge_Labels(Element, Majors));
		lv_obj_set_style_text_font(Scale, &dash_font_numeral_160, LV_PART_INDICATOR);
		lv_obj_set_style_text_color(Scale, Mark, LV_PART_INDICATOR);
		lv_obj_set_style_text_opa(Scale, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_pad_radial(Scale,
		                            S(UI_GAUGE_NUMERAL_PAD + LV_SCALE_LABEL_GAP)
		                            - LV_SCALE_LABEL_GAP,
		                            LV_PART_INDICATOR);
		lv_scale_set_label_show(Scale, true);
	}

	/* Medium ticks halfway between majors: a second ring counting in halves,
	   whose majors are invisible since the first ring already draws them. */
	Half = UiGauge_TickRing(Parent, Sweep, X, Y, W, H, ((Majors - 1u) * 2u) + 1u, 2u);
	lv_obj_set_style_line_width(Half, 0, LV_PART_INDICATOR);
	lv_obj_set_style_length(Half, S(UI_GAUGE_HALF_LENGTH), LV_PART_ITEMS);
	lv_obj_set_style_line_width(Half, S(UI_GAUGE_HALF_WIDTH), LV_PART_ITEMS);
	lv_obj_set_style_line_color(Half, Mark, LV_PART_ITEMS);
	lv_obj_set_style_line_opa(Half, LV_OPA_COVER, LV_PART_ITEMS);

	/* The legend: under the centre on a full dial, where the MR2 prints
	   "x1000r/min"; beyond its reading on a split one. A child of the scale
	   with an ordinary alignment, which LVGL resolves at layout -
	   lv_obj_align_to() would place it against the scale's coordinates as they
	   stand now, before the scale has been laid out at all. */
	if (Element->Legend != NULL)
	{
		lv_obj_t *Legend = lv_label_create(Scale);
		int32_t Radius = UiGauge_Radius(W / UI_GAUGE_RENDER_SCALE,
		                                H / UI_GAUGE_RENDER_SCALE);

		lv_obj_remove_style_all(Legend);
		lv_label_set_text(Legend, Element->Legend);
		lv_obj_set_style_text_font(Legend, &dash_font_legend_104, LV_PART_MAIN);
		lv_obj_set_style_text_color(Legend, Mark, LV_PART_MAIN);
		lv_obj_set_style_text_opa(Legend, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_align(Legend, LV_ALIGN_CENTER, 0, S(UiGauge_LegendDy(Sweep, Radius)));
	}

	return Scale;
}
