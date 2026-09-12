/*
 * lv_conf.h - LVGL configuration for the dash node.
 *
 * Deliberately minimal. LVGL's lv_conf_internal.h supplies a default for every
 * option it knows about, so this file lists only the settings where the default
 * is wrong for this board - which makes each line here a decision someone made
 * rather than a line inherited from a template.
 *
 * Found by CMake through -DLV_CONF_PATH, not by being next to the lvgl/
 * directory: LVGL lives in external/ (ignored, cloned per machine) and this
 * belongs with the firmware that depends on it.
 *
 * LVGL 8.3.x. Not 9: the vendor CO5300 and CST9217 drivers are written against
 * 8.1 and the v8 API is what src/ui_lvgl.c uses.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*---------------------------------------------------------------------------*/
/* Colour                                                                    */
/*---------------------------------------------------------------------------*/

/* RGB565, matching the panel's pixel format. */
#define LV_COLOR_DEPTH		16

/* THE PANEL WANTS THE HIGH BYTE FIRST.
 *
 * The CO5300 takes RGB565 most-significant byte first on the wire, and the
 * flush hands LVGL's buffer straight to DMA with no byte swap in between - so
 * LVGL has to store it pre-swapped. The vendor driver's own
 * AMOLED_1IN75_Clear() shows the convention: it builds its fill word as
 * `Color>>8 | (Color&0xff)<<8`.
 *
 * Get this wrong and nothing fails - the gauges simply render in wrong
 * colours, which is easy to mistake for a styling mistake. */
#define LV_COLOR_16_SWAP	1

/*---------------------------------------------------------------------------*/
/* Memory                                                                    */
/*---------------------------------------------------------------------------*/

/* LVGL's own heap, for the object tree. The tree here is small - at most eight
 * widgets on a face, rebuilt only on a page change (see src/ui_lvgl.c) - but
 * lv_chart's point arrays come out of here too, and running out shows up as
 * widgets silently failing to appear rather than as a crash.
 *
 * This is separate from the draw buffers, which are static arrays in panel.c
 * and deliberately not malloc'd. */
#define LV_MEM_CUSTOM		0
#define LV_MEM_SIZE		(48U * 1024U)

/*---------------------------------------------------------------------------*/
/* Timing                                                                    */
/*---------------------------------------------------------------------------*/

/* Take the tick from the SDK's own clock rather than running a repeating timer
 * to call lv_tick_inc(). The vendor example uses a 5 ms timer; that is one
 * more interrupt competing with can2040 for latency, and it drifts if a tick
 * is ever missed. Reading the time when LVGL asks for it cannot drift. */
#define LV_TICK_CUSTOM			1
#define LV_TICK_CUSTOM_INCLUDE		"pico/time.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR	(to_ms_since_boot(get_absolute_time()))

/* How often LVGL looks for something to redraw. The fastest telemetry tier is
 * 20 ms (PLAN.md section 3), so redrawing faster than that only costs panel
 * DMA bandwidth - which is the resource can2040 is competing for. */
#define LV_DISP_DEF_REFR_PERIOD		20

/* Touch sampling. A swipe has to be sampled several times across its travel
 * for LVGL to call it a gesture rather than a tap, so the default 30 ms is
 * loosened a little; below about 15 ms the I2C traffic starts to matter. */
#define LV_INDEV_DEF_READ_PERIOD	20

/*---------------------------------------------------------------------------*/
/* No floating point, anywhere                                               */
/*---------------------------------------------------------------------------*/

/* The whole signal path is fixed-point integer by design - the SAMC21 scales
 * raw ECU values into hundredths before they reach the bus, and
 * src/signals.c formats them by inserting the decimal point by hand. Letting
 * a float into the print path here would undo that for no gain, and the
 * RP2350's Cortex-M33 FPU is single precision only. */
#define LV_SPRINTF_CUSTOM	0
#define LV_SPRINTF_USE_FLOAT	0

/*---------------------------------------------------------------------------*/
/* Diagnostics                                                               */
/*---------------------------------------------------------------------------*/

/* Warnings and worse go to the USB console. Cheap, and this is a bring-up
 * board. */
#define LV_USE_LOG		1
#define LV_LOG_LEVEL		LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF		1

/* Keep the allocation and null assertions on. They are a few bytes of flash
 * and they catch exactly the class of mistake found in the vendor's own LVGL
 * example - a draw buffer sized in the wrong unit (see vendor/README.md). The
 * style assertion is the expensive one and stays off. */
#define LV_USE_ASSERT_NULL	1
#define LV_USE_ASSERT_MALLOC	1
#define LV_USE_ASSERT_STYLE	0

/* Frame time and CPU load, drawn as an overlay. Off because it sits on top of
 * the gauge, but this is the measurement M2 and M4 ask for - turn it on rather
 * than instrumenting the flush by hand. */
#define LV_USE_PERF_MONITOR	0
#define LV_USE_MEM_MONITOR	0

/*---------------------------------------------------------------------------*/
/* Fonts                                                                     */
/*---------------------------------------------------------------------------*/

/* A 2 inch gauge read at arm's length across a dashboard needs large digits,
 * so the value text gets 48 px and the signal name 14. Fonts cost flash, not
 * RAM, and there is 16 MB of it. */
#define LV_FONT_MONTSERRAT_14	1
#define LV_FONT_MONTSERRAT_28	1
#define LV_FONT_MONTSERRAT_48	1
#define LV_FONT_DEFAULT		&lv_font_montserrat_28

/*---------------------------------------------------------------------------*/
/* Widgets                                                                   */
/*---------------------------------------------------------------------------*/

/* src/ui_lvgl.c builds arcs, bars, charts and labels and nothing else - those
 * are the four shapes the page tables can describe (see src/pages.h). The rest
 * are off to keep the image small; a WIDGET_* type added to the page tables
 * needs its widget turned back on here, and the build will say so. */
#define LV_USE_ANIMIMG		0
#define LV_USE_CALENDAR		0
#define LV_USE_CANVAS		0
#define LV_USE_CHECKBOX		0
#define LV_USE_COLORWHEEL	0
#define LV_USE_DROPDOWN		0
#define LV_USE_IMGBTN		0
#define LV_USE_KEYBOARD		0
#define LV_USE_LED		0
#define LV_USE_LIST		0
#define LV_USE_MENU		0
#define LV_USE_MSGBOX		0
#define LV_USE_ROLLER		0
#define LV_USE_SPINBOX		0
#define LV_USE_SPINNER		0
#define LV_USE_TABVIEW		0
#define LV_USE_TEXTAREA		0
#define LV_USE_TILEVIEW		0
#define LV_USE_WIN		0

#endif /* LV_CONF_H */
