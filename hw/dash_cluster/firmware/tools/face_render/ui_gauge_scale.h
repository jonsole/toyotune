/*
 * ui_gauge_scale.h
 *
 * The LVGL half of the gauge dial - the half that exists only on the PC.
 *
 * LVGL is not linked into the firmware any more (see
 * hw/dash_cluster/RENDERER_PLAN.md). It is still the right tool for DESIGNING
 * a dial, where a tick ring costing two thirds of a frame does not matter
 * because there are no frames: the renderer builds the gauge once, snapshots
 * it, and writes the picture into src/dash_faces.c.
 *
 * The geometry is NOT here. It is in firmware/src/ui_gauge.h, which this
 * includes, because the firmware draws the needle live and it has to land on
 * these graduations to the pixel. One copy, both sides.
 */
#ifndef UI_GAUGE_SCALE_H_
#define UI_GAUGE_SCALE_H_

#include "lvgl.h"

#include "pages.h"
#include "ui_gauge.h"

/* HOW MUCH LARGER THAN THE PANEL THE DIAL IS RENDERED.
 *
 * The faces are drawn at four times the panel's resolution and reduced on the
 * way into the firmware, by build_faces.py: each panel pixel is the 4x4 block
 * under it, which gives 16 levels of true area coverage at every edge, and the
 * reduction then chooses how many of those levels the palette may spend. Every
 * pixel dimension in ui_gauge.h is multiplied by this here, so the geometry the
 * firmware's live needle relies on is unchanged, only finer. */
#define UI_GAUGE_RENDER_SCALE	(4)

/* The face every gauge on a page shares: the charcoal disc, the centre ring,
   and on a Split face - one carrying a top and a bottom half - the divider
   between their readings. Once per page, before the gauges. X/Y/W/H are in
   RENDER pixels, UI_GAUGE_RENDER_SCALE times the panel's, and must be exact
   multiples of it. */
extern lv_obj_t *UiGauge_CreateFace(lv_obj_t *Parent, int32_t X, int32_t Y,
                                    int32_t W, int32_t H, bool Split);

/* One gauge's graduations - ticks, numbers, warning band and legend - over its
   sweep, in the NORMAL state's colours, at X/Y/W/H in render pixels on the
   parent. face_render.c snapshots the page these make. */
extern lv_obj_t *UiGauge_CreateScale(lv_obj_t *Parent, const FaceElement_t *Element,
                                     int32_t X, int32_t Y, int32_t W, int32_t H);

#endif /* UI_GAUGE_SCALE_H_ */
