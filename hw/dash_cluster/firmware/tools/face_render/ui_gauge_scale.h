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

/* The dial itself - graduations, numbers, warning band, centre ring and legend
   - as LVGL objects in the NORMAL state's colours, at X/Y/W/H in the parent's
   coordinates. face_render.c snapshots exactly this. */
extern lv_obj_t *UiGauge_CreateScale(lv_obj_t *Parent, const FaceElement_t *Element,
                                     int32_t X, int32_t Y, int32_t W, int32_t H);

#endif /* UI_GAUGE_SCALE_H_ */
