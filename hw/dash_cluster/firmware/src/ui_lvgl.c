/*
 * ui_lvgl.c
 *
 * The LVGL binding: page tables in, LVGL objects out.
 *
 * Deliberately mechanical. Every judgement - staleness, warning bands, needle
 * position, what to show before any data arrives - is made in ui_model.c,
 * which builds and is tested on a host. This file only translates that into
 * lv_arc/lv_label/lv_bar/lv_chart calls, so it can be read and confirmed by
 * eye without a panel attached.
 *
 * LVGL 8.x. The vendor's CO5300 panel driver and CST9217 touch glue are
 * written against 8.1, so that is the version to match until someone
 * deliberately ports both - see PLAN.md section 4.1a.
 *
 * The hardware-specific parts - the flush callback and the touch read - are
 * in panel.c, which is the only file that knows a CO5300 or a CST9217 exists.
 * Nothing here refers to either.
 */

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#include "pages.h"
#include "signals.h"
#include "ui_lvgl.h"
#include "ui_model.h"

/* One LVGL object per element of the current page, rebuilt on a page change
   and only updated in between. Rebuilding every frame would allocate and free
   continuously on a node with no heap to spare. */
#define UI_MAX_ELEMENTS		(8)

/* Enough for the longest formatted value plus its unit - Signal_Format() is
   bounded well under this. */
#define UI_TEXT_MAX		(24)

/* How often a graph advances. A chart is a time series and has to move whether
   or not the reading changed, so it is the one widget that cannot be skipped
   when nothing happens - which makes its cadence a real cost. At 100 points
   this gives ten seconds of history; driving it every 20 ms gave two, and
   redrew five times as often for the privilege. */
#define UI_CHART_PERIOD_MS	(100u)

typedef struct
{
	lv_obj_t *Object;	/* the scale, arc, bar or label itself */
	lv_obj_t *Value;	/* the value text, where the widget has one */
	lv_obj_t *Label;	/* the signal name */
	lv_obj_t *Needle;	/* WIDGET_GAUGE only: the line that swings */
	int32_t NeedleLen;	/* pixels, fixed at build time */

	/* What was last pushed into LVGL, so an unchanged gauge costs nothing.
	   See the note at the top of UiLvgl_Update(). */
	bool Primed;		/* false until every property has been pushed once */
	uint16_t LastPosition;
	UiState_t LastState;
	const char *LastLabel;
	char LastText[UI_TEXT_MAX];

	uint32_t NextChartMs;
} UiObject_t;

static lv_obj_t *Screen;
static UiObject_t Objects[UI_MAX_ELEMENTS];
static uint8_t ObjectCount;
static uint8_t BuiltPage = 0xFF;

/* How the next page change should be presented, set by the gesture that caused
   it and consumed by the rebuild. A change nobody swiped for - the fault
   takeover - stays LV_SCREEN_LOAD_ANIM_NONE deliberately: a warning that slides
   in gently is a warning the driver reads late. */
static lv_screen_load_anim_t PendingAnim = LV_SCREEN_LOAD_ANIM_NONE;

/* Long enough to read as movement, short enough not to feel like waiting.
   Both screens are drawn for this long, so it is also the only moment the
   renderer has two object trees and twice the draw area.

   200 ms read as too fast on the glass, then 350 ms still did. A full-screen
   redraw is the most expensive thing this renderer does, so a short slide is
   also a slide made of very few steps - lengthening it helps twice over. At
   600 ms and roughly 30 frames a second this is about eighteen steps across
   the travel. */
#define UI_TRANSITION_MS	(400u)

/* A CROSSFADE, BECAUSE MOTION CANNOT BE MADE SMOOTH HERE.
 *
 * Three sliding transitions were tried on the glass and all three looked bad,
 * for one reason that no amount of tuning fixes. A full-screen frame costs
 * about 30 ms out of LVGL's software renderer - no 2D acceleration on this
 * part - so 466 pixels of travel is roughly twenty steps of twenty-three
 * pixels. The eye tracks moving content and reads those steps as judder.
 * Shortening the transition gives fewer, bigger steps; lengthening it just
 * prolongs the experience.
 *
 * A fade has nothing to track. The same ten or twenty frames that look
 * stuttery as motion read as a smooth dissolve as brightness, because there
 * are no edges moving across the retina. It costs the same per frame as the
 * slide did - both screens are composited while the new one is part
 * transparent - and looks better for it.
 *
 * What was tried, so nobody repeats it: MOVE (both faces animate) at 350 and
 * 600 ms, OVER (only the incoming face animates, half the per-frame cost) at
 * 600 ms, and a strip-by-strip wipe. The wipe was the cheapest by far - it
 * renders each face exactly once, where a slide renders the moving one every
 * frame - and was rejected on looks, which is the right criterion for an
 * animation.
 *
 * The honest cost of a fade: it has no direction, so a leftward and a
 * rightward swipe look identical and the driver gets no cue which way the
 * carousel moved. The face's own name is the cue instead.
 *
 * If motion is ever wanted for real, the only thing that would work is
 * snapshotting each face once into an 8bpp framebuffer and compositing rows
 * from two of them straight to the panel - around 6 ms a frame, so genuinely
 * 60 fps. It costs 424 KB of the 520 KB SRAM and RGB332 colour, and it lives
 * outside LVGL. Measured numbers behind all of this are in the git history
 * around this commit. */
#define UI_ANIM_PAGE_CHANGE	LV_SCREEN_LOAD_ANIM_FADE_IN

/* Gestures are ignored until this time. Two reasons, and the second is the
   real one: a second swipe mid-transition would ask LVGL to load a third
   screen while the previous one is still being animated out and deleted, and
   on a dashboard a swipe that lands twice is worse than one that is briefly
   ignored. */
static uint32_t TransitionUntilMs;

/* Percent of the panel to pixels. The page tables are in percent so one table
   serves the 1.43" and 1.75" panels, which share 466x466. */
static void UiLvgl_GestureEvent(lv_event_t *Event);

static int32_t Pct(uint8_t Percent, int32_t Extent)
{
	return (int32_t)(((int32_t)Percent * Extent) / 100);
}


/***************************************************************************************/
/* How far a gauge sweeps, and where it starts.
 *
 * 270 degrees beginning at 135 is the car-instrument convention: the needle
 * rests at the lower left, climbs over the top and finishes at the lower
 * right, leaving the bottom of the face clear for the numeric readout. */
#define UI_GAUGE_ANGLE_RANGE	(270u)
#define UI_GAUGE_ROTATION	(135)

/* Minor ticks between one labelled tick and the next. The label list decides
   how many major ticks there are, so the total follows from both. */
#define UI_GAUGE_MINOR_PER_MAJOR	(5u)

/* Needle length as a percent of the gauge's radius - short of the ticks, so
   the tip points at them rather than through them. */
#define UI_GAUGE_NEEDLE_PCT	(72)


/***************************************************************************************/
/* A bare black screen, ready to have a face built on it.
 *
 * One per page rather than one reused, because lv_screen_load_anim() animates
 * between two screens - there is nothing to slide if both faces live on the
 * same one. The old screen is deleted by the load, so only two exist at once
 * and only for UI_TRANSITION_MS. */
static lv_obj_t *UiLvgl_MakeScreen(void)
{
	lv_obj_t *New = lv_obj_create(NULL);

	/* The theme styles a plain object like a card - a light panel, a border and
	   padding. On a round AMOLED in a dashboard the background must be true
	   black, both because anything else is a visible disc and because black
	   costs an AMOLED no light. */
	lv_obj_set_style_bg_color(New, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(New, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(New, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_all(New, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(New, 0, LV_PART_MAIN);

	/* See the note in UiLvgl_BuildElement(): a scroll in progress suppresses
	   gesture detection outright, and LVGL creates everything scrollable. */
	lv_obj_remove_flag(New, LV_OBJ_FLAG_SCROLLABLE);

	/* EVERY screen needs this, not just the first. The gesture lands on the
	   screen because LVGL walks up from the object under the finger while each
	   one has LV_OBJ_FLAG_GESTURE_BUBBLE - default on for children, off for a
	   screen - so the walk stops here. Miss it on a new screen and swiping
	   works exactly once. */
	lv_obj_add_event_cb(New, UiLvgl_GestureEvent, LV_EVENT_GESTURE, NULL);

	return New;
}


/***************************************************************************************/
static void UiLvgl_Style(lv_obj_t *Object, UiState_t State)
{
	lv_color_t Colour;

	switch (State)
	{
	case UI_STATE_WARNING:
		Colour = lv_palette_main(LV_PALETTE_RED);
		break;
	case UI_STATE_STALE:
	case UI_STATE_NODATA:
		/* Dimmed rather than hidden. A driver must be able to see that a gauge
		   exists and is not reporting, which is different from a gauge that is
		   not there. */
		Colour = lv_palette_darken(LV_PALETTE_GREY, 2);
		break;
	case UI_STATE_NORMAL:
	default:
		Colour = lv_color_white();
		break;
	}

	lv_obj_set_style_text_color(Object, Colour, LV_PART_MAIN);
	lv_obj_set_style_arc_color(Object, Colour, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(Object, Colour, LV_PART_INDICATOR);

	/* A scale's ticks and numbers are its MAIN and ITEMS parts, so the state
	   colour has to reach those too or a stale gauge would keep bright white
	   graduations around a dimmed needle. */
	lv_obj_set_style_line_color(Object, Colour, LV_PART_MAIN);
	lv_obj_set_style_line_color(Object, Colour, LV_PART_ITEMS);
	lv_obj_set_style_text_color(Object, Colour, LV_PART_INDICATOR);
}


/***************************************************************************************/
static void UiLvgl_BuildElement(const FaceElement_t *Element, UiObject_t *Out,
                                int32_t W, int32_t H)
{
	int32_t X = Pct(Element->X, W);
	int32_t Y = Pct(Element->Y, H);
	int32_t EW = Pct(Element->W, W);
	int32_t EH = Pct(Element->H, H);

	switch (Element->Type)
	{
	case WIDGET_GAUGE:
	{
		/* lv_scale is v9's replacement for v8's lv_meter, which no longer
		   exists. It draws the ticks and the numbers and works out where the
		   needle has to point; the needle itself is an ordinary line. */
		uint32_t Majors = 0;
		int32_t Radius;

		Out->Object = lv_scale_create(Screen);
		lv_obj_set_pos(Out->Object, X, Y);
		lv_obj_set_size(Out->Object, EW, EH);
		lv_scale_set_mode(Out->Object, LV_SCALE_MODE_ROUND_INNER);
		lv_scale_set_angle_range(Out->Object, UI_GAUGE_ANGLE_RANGE);
		lv_scale_set_rotation(Out->Object, UI_GAUGE_ROTATION);

		/* The needle is driven with the model's normalised position, so the
		   scale counts in those units too and the printed numbers come from
		   the label list instead. One less place for a range to disagree. */
		lv_scale_set_range(Out->Object, 0, UI_POSITION_MAX);

		if (Element->Ticks != NULL)
		{
			while (Element->Ticks[Majors] != NULL)
				Majors++;
			lv_scale_set_text_src(Out->Object, (const char **)Element->Ticks);
		}

		if (Majors < 2u)
			Majors = 2u;	/* a scale with one tick is not a scale */

		lv_scale_set_total_tick_count(Out->Object,
		                              ((Majors - 1u) * UI_GAUGE_MINOR_PER_MAJOR) + 1u);
		lv_scale_set_major_tick_every(Out->Object, UI_GAUGE_MINOR_PER_MAJOR);
		lv_scale_set_label_show(Out->Object, true);

		Radius = (int32_t)((EW < EH ? EW : EH) / 2);
		Out->NeedleLen = (Radius * UI_GAUGE_NEEDLE_PCT) / 100;

		Out->Needle = lv_line_create(Out->Object);
		lv_obj_set_style_line_width(Out->Needle, 5, LV_PART_MAIN);
		lv_obj_set_style_line_rounded(Out->Needle, true, LV_PART_MAIN);
		lv_obj_set_style_line_color(Out->Needle,
		                            lv_palette_main(LV_PALETTE_RED),
		                            LV_PART_MAIN);
		break;
	}

	case WIDGET_DIAL:
	case WIDGET_ARC:
		Out->Object = lv_arc_create(Screen);
		lv_obj_set_pos(Out->Object, X, Y);
		lv_obj_set_size(Out->Object, EW, EH);
		lv_arc_set_range(Out->Object, 0, UI_POSITION_MAX);
		/* A dial sweeps most of the circle; an arc is a shallower indicator
		   used as a secondary reading on the same face. */
		if (Element->Type == WIDGET_DIAL)
			lv_arc_set_bg_angles(Out->Object, 135, 45);
		else
			lv_arc_set_bg_angles(Out->Object, 160, 20);
		lv_obj_remove_style(Out->Object, NULL, LV_PART_KNOB);
		lv_obj_remove_flag(Out->Object, LV_OBJ_FLAG_CLICKABLE);
		break;

	case WIDGET_BARGRAPH:
		Out->Object = lv_bar_create(Screen);
		lv_obj_set_pos(Out->Object, X, Y);
		lv_obj_set_size(Out->Object, EW, EH);
		lv_bar_set_range(Out->Object, 0, UI_POSITION_MAX);
		break;

	case WIDGET_GRAPH:
		Out->Object = lv_chart_create(Screen);
		lv_obj_set_pos(Out->Object, X, Y);
		lv_obj_set_size(Out->Object, EW, EH);
		lv_chart_set_type(Out->Object, LV_CHART_TYPE_LINE);
		lv_chart_set_range(Out->Object, LV_CHART_AXIS_PRIMARY_Y, 0, UI_POSITION_MAX);
		lv_chart_set_point_count(Out->Object, 100);
		lv_chart_add_series(Out->Object, lv_palette_main(LV_PALETTE_AMBER),
		                    LV_CHART_AXIS_PRIMARY_Y);
		break;

	case WIDGET_NUMERIC:
	default:
		Out->Object = lv_obj_create(Screen);
		lv_obj_set_pos(Out->Object, X, Y);
		lv_obj_set_size(Out->Object, EW, EH);
		lv_obj_set_style_bg_opa(Out->Object, LV_OPA_TRANSP, LV_PART_MAIN);
		lv_obj_set_style_border_width(Out->Object, 0, LV_PART_MAIN);
		break;
	}

	Out->Value = lv_label_create(Out->Object);
	lv_obj_align(Out->Value, LV_ALIGN_CENTER, 0, 0);
	lv_label_set_text(Out->Value, "--");

	Out->Label = lv_label_create(Out->Object);
	lv_obj_align(Out->Label, LV_ALIGN_BOTTOM_MID, 0, 0);

	/* Nothing has been pushed into this object yet, so the first update writes
	   every property regardless of what it is compared against. Cheaper and
	   far clearer than seeding each cache with a value the model can never
	   produce. */
	Out->Primed = false;
	Out->LastPosition = 0;
	Out->Needle = NULL;
	Out->NeedleLen = 0;
	Out->LastState = UI_STATE_NORMAL;
	Out->LastLabel = NULL;
	Out->LastText[0] = '\0';
	Out->NextChartMs = 0;

	/* NOTHING ON A FACE MAY SCROLL.
	 *
	 * LVGL creates every object scrollable, and its gesture detection gives
	 * up the moment a scrollable object under the finger starts scrolling -
	 * indev_gesture() returns early if a scroll is in progress. A widget
	 * whose content sits a pixel outside its bounds is enough to make that
	 * happen, and the symptom is a swipe that sometimes changes page and
	 * sometimes does not. So the flag comes off every widget, and off the
	 * screen itself in UiLvgl_Init().
	 *
	 * Gesture bubbling is left alone: LVGL sets it on every child by default
	 * and walks up to the first ancestor without it, which is the screen. */
	lv_obj_remove_flag(Out->Object, LV_OBJ_FLAG_SCROLLABLE);
}


/***************************************************************************************/
static void UiLvgl_BuildPage(uint8_t Page)
{
	const FacePage_t *Face = &Pages[Page];

	/* From the display, not from the screen object. A screen that has just
	   been created and not yet laid out can report a width of zero, and every
	   element is positioned as a percentage of it - so reading it from the
	   object would collapse the whole face to nothing on exactly the page
	   changes this function exists to handle. */
	int32_t W = lv_display_get_horizontal_resolution(NULL);
	int32_t H = lv_display_get_vertical_resolution(NULL);
	lv_obj_t *New = UiLvgl_MakeScreen();
	uint8_t i;

	ObjectCount = Face->ElementCount;
	if (ObjectCount > UI_MAX_ELEMENTS)
		ObjectCount = UI_MAX_ELEMENTS;

	Screen = New;

	for (i = 0; i < ObjectCount; i++)
		UiLvgl_BuildElement(&Face->Elements[i], &Objects[i], W, H);

	/* auto_del: the screen being replaced is deleted once the animation
	   finishes, which is what keeps this from leaking a face per swipe. Its
	   widgets are still being drawn as they slide away, holding whatever
	   values they last had - correct, since they are leaving. */
	lv_screen_load_anim(New, PendingAnim,
	                 (PendingAnim == LV_SCREEN_LOAD_ANIM_NONE)
	                         ? 0 : (uint32_t)UI_TRANSITION_MS,
	                 0, true);

	if (PendingAnim != LV_SCREEN_LOAD_ANIM_NONE)
		TransitionUntilMs = lv_tick_get() + UI_TRANSITION_MS;

	/* Back to no animation, so a page change from anywhere other than a
	   gesture is instant rather than inheriting the last swipe's direction. */
	PendingAnim = LV_SCREEN_LOAD_ANIM_NONE;

	BuiltPage = Page;
}


/***************************************************************************************/
static void UiLvgl_GestureEvent(lv_event_t *Event)
{
	(void)Event;
	UiLvgl_HandleGesture();
}


/***************************************************************************************/
void UiLvgl_Init(void)
{
	/* Black the screen LVGL made for the display. Nothing is built on it - the
	   first UiLvgl_Update() replaces it with a real face - but it is on the
	   glass until then, and the theme's default is a pale card. */
	lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), LV_PART_MAIN);

	Screen = NULL;
	BuiltPage = 0xFF;
	ObjectCount = 0;
	PendingAnim = LV_SCREEN_LOAD_ANIM_NONE;
	TransitionUntilMs = 0;
}


/***************************************************************************************/
/* PUSH ONLY WHAT CHANGED.
 *
 * This ran unconditionally once, and it cost more than everything else in the
 * firmware put together. Measured on the board: a static face with nothing on
 * the bus redrew 187,489 pixels every 60 ms - 433 squared, which is one 90%
 * dial plus its extended draw area, repainted sixteen times a second for no
 * reason at all.
 *
 * LVGL cannot prevent this on its own. lv_arc_set_value() does early-out on an
 * unchanged value, but lv_label_set_text() never compares the string it is
 * handed, and every lv_obj_set_style_*() call invalidates the whole widget -
 * and UiLvgl_Style() makes three of them per element. So the comparison has to
 * happen here.
 *
 * It is worth more than a smoother transition: a dashboard gauge is static
 * most of the time, and a core busy repainting an unchanged dial is a core not
 * available to the renderer during a page change - or, once there is a bus, one
 * competing with can2040 for memory bandwidth.
 */
void UiLvgl_Update(uint32_t NowMs)
{
	uint8_t Page = Pages_Effective(NowMs);
	const FacePage_t *Face;
	uint8_t i;

	if (Page != BuiltPage)
		UiLvgl_BuildPage(Page);
	else if ((int32_t)(lv_tick_get() - TransitionUntilMs) < 0)
		/* Mid-slide. New label text invalidates areas on top of the
		   animation's own invalidation, for values nobody can read while the
		   face is moving. Note this is the `else` branch on purpose: the call
		   that builds a page still falls through and populates it, so a face is
		   never shown holding the placeholder text. */
		return;

	Face = &Pages[Page];

	for (i = 0; i < ObjectCount; i++)
	{
		const FaceElement_t *Element = &Face->Elements[i];
		UiObject_t *O = &Objects[i];
		UiWidget_t Widget = UiModel_Widget(Element, NowMs);
		bool Force = !O->Primed;
		char Text[UI_TEXT_MAX];

		/* LVGL takes a signed coordinate; the model produces an unsigned
		   0..UI_POSITION_MAX, which is 1000 - so the cast cannot lose
		   anything, and the ranges set in UiLvgl_BuildElement() match. */
		int32_t Position = (int32_t)Widget.Position;

		switch (Element->Type)
		{
		case WIDGET_GAUGE:
			if (Force || Widget.Position != O->LastPosition)
				lv_scale_set_line_needle_value(O->Object, O->Needle,
				                               O->NeedleLen, Position);
			break;
		case WIDGET_DIAL:
		case WIDGET_ARC:
			if (Force || Widget.Position != O->LastPosition)
				lv_arc_set_value(O->Object, Position);
			break;
		case WIDGET_BARGRAPH:
			if (Force || Widget.Position != O->LastPosition)
				lv_bar_set_value(O->Object, Position, LV_ANIM_OFF);
			break;
		case WIDGET_GRAPH:
			/* Time driven, not change driven - a series has to advance even
			   when the reading is identical, or the graph stops telling the
			   truth about time. */
			if (Force || (int32_t)(NowMs - O->NextChartMs) >= 0)
			{
				lv_chart_set_next_value(O->Object,
				                        lv_chart_get_series_next(O->Object, NULL),
				                        Position);
				O->NextChartMs = NowMs + UI_CHART_PERIOD_MS;
			}
			break;
		default:
			break;
		}

		O->LastPosition = Widget.Position;

		snprintf(Text, sizeof(Text), "%s %s", Widget.Text, Widget.Unit);
		if (Force || strcmp(Text, O->LastText) != 0)
		{
			lv_label_set_text(O->Value, Text);
			(void)snprintf(O->LastText, sizeof(O->LastText), "%s", Text);
		}

		/* The model returns a pointer to a fixed string, so identity is
		   enough - and the name of a signal does not change within a face
		   anyway. */
		if (Force || Widget.Label != O->LastLabel)
		{
			lv_label_set_text(O->Label, Widget.Label);
			O->LastLabel = Widget.Label;
		}

		if (Force || Widget.State != O->LastState)
		{
			UiLvgl_Style(O->Object, Widget.State);
			O->LastState = Widget.State;
		}

		O->Primed = true;
	}
}


/***************************************************************************************/
/* Swipe handling. A deliberate gesture only: LVGL reports a direction once the
   touch has travelled far enough, which is what keeps a wet fingertip or a
   bump from changing page. A tap is deliberately ignored. */
void UiLvgl_HandleGesture(void)
{
	lv_dir_t Dir = lv_indev_get_gesture_dir(lv_indev_active());

	/* Signed difference, the same wrap-safe deadline test used in can_link.c:
	   negative means the deadline is still ahead. */
	if ((int32_t)(lv_tick_get() - TransitionUntilMs) < 0)
		return;

	if (Dir == LV_DIR_LEFT)
	{
		Pages_Next();
		PendingAnim = UI_ANIM_PAGE_CHANGE;
	}
	else if (Dir == LV_DIR_RIGHT)
	{
		Pages_Previous();
		PendingAnim = UI_ANIM_PAGE_CHANGE;
	}
}
