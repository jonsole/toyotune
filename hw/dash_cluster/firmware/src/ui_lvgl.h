/*
 * ui_lvgl.h
 *
 * The LVGL binding. Compiled only when an LVGL checkout is present - see the
 * CMakeLists. Everything it decides lives in ui_model.c, which builds without
 * LVGL and is tested on a host.
 */

#ifndef UI_LVGL_H_
#define UI_LVGL_H_

#include <stdint.h>

/* Call once, after lv_init() and the display driver are up. */
extern void UiLvgl_Init(void);

/* Call from the render loop on core 1. Rebuilds the object tree on a page
   change, and only updates values in between. */
extern void UiLvgl_Update(uint32_t NowMs);

/* Call from an LV_EVENT_GESTURE handler. */
extern void UiLvgl_HandleGesture(void);

#endif /* UI_LVGL_H_ */
