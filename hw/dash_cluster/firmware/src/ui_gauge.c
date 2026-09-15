/*
 * ui_gauge.c
 *
 * The gauge dial, styled after the SW20 MR2's own instruments: a charcoal face,
 * heavy warm-white graduations every major with medium ones halfway and small
 * ones between, bold condensed numerals inside them, a solid red warning band
 * at the top of the range, and a legend under the centre.
 *
 * Compiled into two programs: the face renderer on the PC, which snapshots it
 * into src/dash_faces.c, and the firmware, which only builds it live as a
 * fallback. See ui_gauge.h. Anything that changes how the dial looks belongs
 * here - and after changing it, rerun tools/face_render/build_faces.py, or the
 * firmware will keep showing the old picture.
 */

#include "ui_gauge.h"
#include "ui_model.h"

LV_FONT_DECLARE(dash_font_numeral_40);
LV_FONT_DECLARE(dash_font_legend_26);

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

/* One ring of tick marks. The graduations are two of these laid over each
   other, because lv_scale knows only "major" and "minor" and the MR2 has three
   weights of tick. Every theme style comes off first, so nothing - the rim line
   the theme draws, its colours, its lengths - leaks into the look. */
static lv_obj_t *UiGauge_TickRing(lv_obj_t *Parent, int32_t X, int32_t Y,
                                  int32_t W, int32_t H, uint32_t Total,
                                  uint32_t MajorEvery)
{
	lv_obj_t *Scale = lv_scale_create(Parent);

	lv_obj_remove_style_all(Scale);
	lv_obj_set_pos(Scale, X, Y);
	lv_obj_set_size(Scale, W, H);
	lv_scale_set_mode(Scale, LV_SCALE_MODE_ROUND_INNER);
	lv_scale_set_angle_range(Scale, UI_GAUGE_ANGLE_RANGE);
	lv_scale_set_rotation(Scale, UI_GAUGE_ROTATION);

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

lv_obj_t *UiGauge_CreateScale(lv_obj_t *Parent, const FaceElement_t *Element,
                              int32_t X, int32_t Y, int32_t W, int32_t H)
{
	lv_color_t Mark = lv_color_hex(UI_GAUGE_MARK_COLOUR);
	uint32_t Majors = 0;
	lv_obj_t *Scale;
	lv_obj_t *Half;

	if (Element->Ticks != NULL)
		while (Element->Ticks[Majors] != NULL)
			Majors++;
	if (Majors < 2u)
		Majors = 2u;	/* a scale with one tick is not a scale */

	/* Drawn in creation order, back to front: face, band, ticks, centre ring,
	   legend. The band has to sit under the ticks, so it cannot be a child of
	   the scale - children draw after their parent. */

	/* The face: a charcoal disc rather than bare black, as a lit instrument
	   face is. Costs the AMOLED a little light for a lot of look. */
	{
		lv_obj_t *Face = lv_obj_create(Parent);

		lv_obj_remove_style_all(Face);
		lv_obj_set_pos(Face, X, Y);
		lv_obj_set_size(Face, W, H);
		lv_obj_set_style_radius(Face, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_color(Face, lv_color_hex(UI_GAUGE_FACE_COLOUR), LV_PART_MAIN);
		lv_obj_set_style_bg_opa(Face, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_remove_flag(Face, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	}

	/* The warning band: a solid red arc at the rim from BandFrom to the end of
	   the scale, as wide as a major tick is long, so the graduations cross it.
	   Angles are in the scale's own convention - 0 at three o'clock, clockwise -
	   which is lv_arc's too. Painted only: whether the gauge itself turns to its
	   warning state is UiModel_SignalWarning()'s decision, not this. */
	if (Element->BandFrom > Element->Min && Element->BandFrom < Element->Max)
	{
		lv_obj_t *Band = lv_arc_create(Parent);
		int32_t From = UI_GAUGE_ROTATION
		               + (int32_t)((UI_GAUGE_ANGLE_RANGE
		                            * (uint32_t)UiGauge_Position(Element, Element->BandFrom))
		                           / UI_POSITION_MAX);
		int32_t To = UI_GAUGE_ROTATION + (int32_t)UI_GAUGE_ANGLE_RANGE;

		lv_obj_remove_style_all(Band);
		lv_obj_set_pos(Band, X, Y);
		lv_obj_set_size(Band, W, H);
		lv_arc_set_bg_angles(Band, From % 360, To % 360);
		lv_obj_set_style_arc_width(Band, UI_GAUGE_MAJOR_LENGTH, LV_PART_MAIN);
		lv_obj_set_style_arc_color(Band, lv_color_hex(UI_GAUGE_BAND_COLOUR), LV_PART_MAIN);
		lv_obj_set_style_arc_opa(Band, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_set_style_arc_rounded(Band, false, LV_PART_MAIN);
		lv_obj_remove_flag(Band, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	}

	/* Heavy ticks every major and small ones every minor, with the numerals.
	   This is the object the firmware hangs the needle and readout on. */
	Scale = UiGauge_TickRing(Parent, X, Y, W, H,
	                         ((Majors - 1u) * UI_GAUGE_MINOR_PER_MAJOR) + 1u,
	                         UI_GAUGE_MINOR_PER_MAJOR);

	lv_obj_set_style_length(Scale, UI_GAUGE_MAJOR_LENGTH, LV_PART_INDICATOR);
	lv_obj_set_style_line_width(Scale, UI_GAUGE_MAJOR_WIDTH, LV_PART_INDICATOR);
	lv_obj_set_style_line_color(Scale, Mark, LV_PART_INDICATOR);
	lv_obj_set_style_line_opa(Scale, LV_OPA_COVER, LV_PART_INDICATOR);

	lv_obj_set_style_length(Scale, UI_GAUGE_MINOR_LENGTH, LV_PART_ITEMS);
	lv_obj_set_style_line_width(Scale, UI_GAUGE_MINOR_WIDTH, LV_PART_ITEMS);
	lv_obj_set_style_line_color(Scale, Mark, LV_PART_ITEMS);
	lv_obj_set_style_line_opa(Scale, LV_OPA_COVER, LV_PART_ITEMS);

	if (Element->Ticks != NULL)
	{
		lv_scale_set_text_src(Scale, (const char **)Element->Ticks);
		lv_obj_set_style_text_font(Scale, &dash_font_numeral_40, LV_PART_INDICATOR);
		lv_obj_set_style_text_color(Scale, Mark, LV_PART_INDICATOR);
		lv_obj_set_style_text_opa(Scale, LV_OPA_COVER, LV_PART_INDICATOR);
		lv_obj_set_style_pad_radial(Scale, UI_GAUGE_NUMERAL_PAD, LV_PART_INDICATOR);
		lv_scale_set_label_show(Scale, true);
	}

	/* Medium ticks halfway between majors: a second ring counting in halves,
	   whose majors are invisible since the first ring already draws them. */
	Half = UiGauge_TickRing(Parent, X, Y, W, H, ((Majors - 1u) * 2u) + 1u, 2u);
	lv_obj_set_style_line_width(Half, 0, LV_PART_INDICATOR);
	lv_obj_set_style_length(Half, UI_GAUGE_HALF_LENGTH, LV_PART_ITEMS);
	lv_obj_set_style_line_width(Half, UI_GAUGE_HALF_WIDTH, LV_PART_ITEMS);
	lv_obj_set_style_line_color(Half, Mark, LV_PART_ITEMS);
	lv_obj_set_style_line_opa(Half, LV_OPA_COVER, LV_PART_ITEMS);

	/* The ring round the centre readout: a bordered circle with nothing inside,
	   its outer edge exactly where the needle's rounded inner end stops - see
	   UiGauge_RingRadius(). A border is drawn inside its object's box, so a box
	   of the ring's outer diameter puts the stroke's outer edge on that radius.
	   A child of the scale only to share its centre. */
	{
		int32_t RingD = 2 * UiGauge_RingRadius(W, H);
		lv_obj_t *Ring = lv_obj_create(Scale);

		lv_obj_remove_style_all(Ring);
		lv_obj_set_size(Ring, RingD, RingD);
		lv_obj_align(Ring, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_style_radius(Ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
		lv_obj_set_style_bg_opa(Ring, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(Ring, UI_GAUGE_RING_WIDTH, LV_PART_MAIN);
		lv_obj_set_style_border_color(Ring, lv_color_hex(UI_GAUGE_RING_COLOUR),
		                              LV_PART_MAIN);
		lv_obj_set_style_border_opa(Ring, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_remove_flag(Ring, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
	}

	/* The legend under the centre, where the MR2 prints "x1000r/min": below
	   the ring and above the gap between the lowest two numerals. A child of
	   the scale with an ordinary alignment, which LVGL resolves at layout -
	   lv_obj_align_to() would place it against the scale's coordinates as they
	   stand now, before the scale has been laid out at all. */
	if (Element->Legend != NULL)
	{
		lv_obj_t *Legend = lv_label_create(Scale);

		lv_obj_remove_style_all(Legend);
		lv_label_set_text(Legend, Element->Legend);
		lv_obj_set_style_text_font(Legend, &dash_font_legend_26, LV_PART_MAIN);
		lv_obj_set_style_text_color(Legend, Mark, LV_PART_MAIN);
		lv_obj_set_style_text_opa(Legend, LV_OPA_COVER, LV_PART_MAIN);
		lv_obj_align(Legend, LV_ALIGN_CENTER, 0,
		             (UiGauge_Radius(W, H) * UI_GAUGE_LEGEND_PCT) / 100);
	}

	return Scale;
}
