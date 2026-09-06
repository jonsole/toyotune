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
 * WHAT IS STILL MISSING
 *
 * The flush callback and touch read callback, which are the only genuinely
 * hardware-specific parts. They come from Waveshare's driver and need the
 * board to test. Everything above them is here.
 */

#include <stdio.h>

#include "lvgl.h"

#include "pages.h"
#include "signals.h"
#include "ui_lvgl.h"
#include "ui_model.h"

/* One LVGL object per element of the current page, rebuilt on a page change
   and only updated in between. Rebuilding every frame would allocate and free
   continuously on a node with no heap to spare. */
#define UI_MAX_ELEMENTS		(8)

typedef struct
{
	lv_obj_t *Object;	/* the arc, bar or label itself */
	lv_obj_t *Value;	/* the value text, where the widget has one */
	lv_obj_t *Label;	/* the signal name */
} UiObject_t;

static lv_obj_t *Screen;
static UiObject_t Objects[UI_MAX_ELEMENTS];
static uint8_t ObjectCount;
static uint8_t BuiltPage = 0xFF;

/* Percent of the panel to pixels. The page tables are in percent so one table
   serves the 1.43" and 1.75" panels, which share 466x466. */
static lv_coord_t Pct(uint8_t Percent, lv_coord_t Extent)
{
	return (lv_coord_t)(((int32_t)Percent * Extent) / 100);
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
}


/***************************************************************************************/
static void UiLvgl_BuildElement(const FaceElement_t *Element, UiObject_t *Out,
                                lv_coord_t W, lv_coord_t H)
{
	lv_coord_t X = Pct(Element->X, W);
	lv_coord_t Y = Pct(Element->Y, H);
	lv_coord_t EW = Pct(Element->W, W);
	lv_coord_t EH = Pct(Element->H, H);

	switch (Element->Type)
	{
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
		lv_obj_clear_flag(Out->Object, LV_OBJ_FLAG_CLICKABLE);
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
}


/***************************************************************************************/
static void UiLvgl_BuildPage(uint8_t Page)
{
	const FacePage_t *Face = &Pages[Page];
	lv_coord_t W = lv_obj_get_width(Screen);
	lv_coord_t H = lv_obj_get_height(Screen);
	uint8_t i;

	lv_obj_clean(Screen);
	ObjectCount = Face->ElementCount;
	if (ObjectCount > UI_MAX_ELEMENTS)
		ObjectCount = UI_MAX_ELEMENTS;

	for (i = 0; i < ObjectCount; i++)
		UiLvgl_BuildElement(&Face->Elements[i], &Objects[i], W, H);

	BuiltPage = Page;
}


/***************************************************************************************/
void UiLvgl_Init(void)
{
	Screen = lv_scr_act();
	lv_obj_set_style_bg_color(Screen, lv_color_black(), LV_PART_MAIN);
	BuiltPage = 0xFF;
	ObjectCount = 0;
}


/***************************************************************************************/
void UiLvgl_Update(uint32_t NowMs)
{
	uint8_t Page = Pages_Effective(NowMs);
	const FacePage_t *Face;
	uint8_t i;

	if (Page != BuiltPage)
		UiLvgl_BuildPage(Page);

	Face = &Pages[Page];

	for (i = 0; i < ObjectCount; i++)
	{
		const FaceElement_t *Element = &Face->Elements[i];
		UiObject_t *O = &Objects[i];
		UiWidget_t Widget = UiModel_Widget(Element, NowMs);
		char Text[24];

		switch (Element->Type)
		{
		case WIDGET_DIAL:
		case WIDGET_ARC:
			lv_arc_set_value(O->Object, Widget.Position);
			break;
		case WIDGET_BARGRAPH:
			lv_bar_set_value(O->Object, Widget.Position, LV_ANIM_OFF);
			break;
		case WIDGET_GRAPH:
			lv_chart_set_next_value(O->Object,
			                        lv_chart_get_series_next(O->Object, NULL),
			                        Widget.Position);
			break;
		default:
			break;
		}

		snprintf(Text, sizeof(Text), "%s %s", Widget.Text, Widget.Unit);
		lv_label_set_text(O->Value, Text);
		lv_label_set_text(O->Label, Widget.Label);
		UiLvgl_Style(O->Object, Widget.State);
	}
}


/***************************************************************************************/
/* Swipe handling. A deliberate gesture only: LVGL reports a direction once the
   touch has travelled far enough, which is what keeps a wet fingertip or a
   bump from changing page. A tap is deliberately ignored. */
void UiLvgl_HandleGesture(void)
{
	lv_dir_t Dir = lv_indev_get_gesture_dir(lv_indev_get_act());

	if (Dir == LV_DIR_LEFT)
		Pages_Next();
	else if (Dir == LV_DIR_RIGHT)
		Pages_Previous();
}
