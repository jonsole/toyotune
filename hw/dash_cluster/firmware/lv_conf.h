/*
 * lv_conf.h - LVGL configuration for the dash node.
 *
 * Deliberately minimal. LVGL's lv_conf_internal.h supplies a default for every
 * option it knows about, so this file lists only the settings where the default
 * is wrong for this board - which makes each line here a decision someone made
 * rather than a line inherited from a template.
 *
 * Found by CMake through LV_BUILD_CONF_PATH, not by being next to the lvgl/
 * directory: LVGL lives in external/ (gitignored, cloned per machine) and this
 * belongs with the firmware that depends on it.
 *
 * LVGL 9.5.0. firmware/README.md records what the v8 to v9 move changed, and
 * why several things that were configuration in v8 are runtime calls in
 * panel.c now.
 */

#ifndef LV_CONF_H
#define LV_CONF_H

/* NOTHING MAY BE #included HERE UNGUARDED.
 *
 * v9 preprocesses this file from assembly sources too - its Helium blend
 * routine is a .S that pulls in lv_conf_internal.h - so a C header included at
 * this point is fed to the assembler and produces a wall of "bad instruction
 * typedef signed char __int8_t". LVGL's own template says as much: put any
 * include inside an __ASSEMBLY__ guard.
 *
 * v8 tolerated the <stdint.h> that used to be here because it had no assembly
 * sources. Nothing below needs it. */

/*---------------------------------------------------------------------------*/
/* Colour                                                                    */
/*---------------------------------------------------------------------------*/

/* RGB565, matching the panel's COLMOD setting of 0x05. */
#define LV_COLOR_DEPTH		16

/* THE PANEL WANTS THE HIGH BYTE FIRST, AND v9 DOES THAT DIFFERENTLY.
 *
 * v8 had LV_COLOR_16_SWAP here, which made the renderer store every pixel
 * pre-swapped for nothing. v9 removed it, and the replacement its header
 * suggests is to call lv_draw_sw_rgb565_swap() inside the flush - a whole
 * extra pass over the buffer on every flush, 217k pixels for a full screen.
 *
 * The better route, and what panel.c does, is to render straight into the
 * swapped format with lv_display_set_color_format(). RGB565_SWAPPED is a real
 * destination for the software blender - lv_draw_sw_blend.c dispatches it to
 * lv_draw_sw_blend_color_to_rgb565_swapped() - so it costs nothing, exactly as
 * v8 did. That needs this support left on, which is also the default. */
#define LV_DRAW_SW_SUPPORT_RGB565_SWAPPED	1

/*---------------------------------------------------------------------------*/
/* Memory                                                                    */
/*---------------------------------------------------------------------------*/

/* LVGL's own heap, for the object tree. The tree here is small - at most eight
 * widgets on a face, rebuilt only on a page change (see src/ui_lvgl.c) - but
 * lv_chart's point arrays come out of here too, and running out shows up as
 * widgets silently failing to appear rather than as a crash.
 *
 * Separate from the draw buffers, which are static arrays in panel.c and
 * deliberately not malloc'd. v9 defaults to 64 KB; 48 is already generous for
 * this tree, and LV_USE_MEM_MONITOR would say by how much. */
#define LV_USE_STDLIB_MALLOC	LV_STDLIB_BUILTIN
#define LV_MEM_SIZE		(128U * 1024U)

/*---------------------------------------------------------------------------*/
/* Timing                                                                    */
/*---------------------------------------------------------------------------*/

/* How often LVGL looks for something to redraw and - in v9, unlike v8 - also
 * how often input devices are read, because they share this period.
 *
 * A ceiling, not a target: LVGL only redraws what has been invalidated, so a
 * steady gauge costs nothing however low this goes. v9's default of 33 ms
 * would cap a page transition at 30 fps; 10 lets it run as fast as the
 * renderer manages. Reading touch this often is nearly free, because the read
 * callback does nothing at all unless the controller's interrupt has fired. */
#define LV_DEF_REFR_PERIOD	10

/* There is no tick setting here any more. v8 had LV_TICK_CUSTOM pointing at an
 * expression; v9 takes a callback at runtime, so panel.c calls
 * lv_tick_set_cb(). A welcome side effect is that LVGL no longer needs the
 * Pico SDK headers on its include path in order to build. */

/*---------------------------------------------------------------------------*/
/* No floating point, anywhere                                               */
/*---------------------------------------------------------------------------*/

/* The whole signal path is fixed-point integer by design - the SAMC21 scales
 * raw ECU values into hundredths before they reach the bus, and src/signals.c
 * formats them by inserting the decimal point by hand. Letting a float into
 * the print path here would undo that for no gain. v9 defaults this off
 * already; it is stated because it is a decision, not an accident. */
#define LV_USE_FLOAT		0

/*---------------------------------------------------------------------------*/
/* Code placement                                                            */
/*---------------------------------------------------------------------------*/

/* LVGL tags its hottest routines - the blenders, the fill loops - with
 * LV_ATTRIBUTE_FAST_MEM, which expands to nothing by default, so they execute
 * from flash through the XIP cache. Putting them in the SDK's .time_critical
 * section instead has the linker copy them into SRAM at boot, the same place
 * can2040's interrupt path lives.
 *
 * Measured on the glass with the gauge needle sweeping, averaged over ~250
 * refreshes a run: 1.56 -> 1.71 Mpx/s, 47.5 -> 41.8 ms a frame, about 10%.
 * The gain was the same at -O2, which is what makes it believable against a
 * run-to-run spread of about 5%.
 *
 * The price is roughly 115 KB of SRAM - .data grows to 123 KB - because every
 * tagged function shares one section name, so --gc-sections can only discard
 * a whole object file's worth and unused blenders come along. Worth giving
 * each function its own section if SRAM ever gets tight. */
#define LV_ATTRIBUTE_FAST_MEM	__attribute__((section(".time_critical.lvgl")))

/*---------------------------------------------------------------------------*/
/* Diagnostics                                                               */
/*---------------------------------------------------------------------------*/

/* Warnings and worse go to the USB console. Cheap, and this is a bring-up
 * board. */
#define LV_USE_LOG		1
#define LV_LOG_LEVEL		LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF		1

#define LV_USE_ARM2D 1
/* Keep the allocation and null assertions on. They are a few bytes of flash
 * and they catch exactly the class of mistake found in the vendor's own LVGL
 * example - a draw buffer sized in the wrong unit (see vendor/README.md). The
 * style assertion is the expensive one and stays off. */
#define LV_USE_ASSERT_NULL	1
#define LV_USE_ASSERT_MALLOC	1
#define LV_USE_ASSERT_STYLE	0

/* Object checks: every widget call confirms it was handed a live object of the
 * right class. Off by default in LVGL, and on here because of what its absence
 * cost - a gauge was passed a NULL needle, and with this off LVGL read garbage
 * from low memory rather than refusing, so the fault surfaced as TLSF heap
 * corruption three calls away instead of an assert naming the call. The checks
 * live in the setter APIs, not the draw loop, so they cost little. */
#define LV_USE_ASSERT_OBJ	1

/* Frame rate and CPU load, drawn as an overlay. In v9 both monitors sit behind
 * LV_USE_SYSMON, which has to be turned on first - it was a standalone option
 * in v8.
 *
 * READ THE NUMBERS CAREFULLY, THEY ARE NOT WHAT THEY LOOK LIKE.
 *
 * FPS is not the observed refresh rate. LVGL sums the RENDER TIME of frames
 * that drew more than a threshold of pixels and reports frames per second as
 * though they had been rendered back to back - so it measures render
 * throughput, and is capped at 1000 / LV_DEF_REFR_PERIOD, which is 100 here.
 * When nothing was drawn at all it reports that cap rather than zero, so now
 * that unchanged widgets are never repainted (see src/ui_lvgl.c) a still gauge
 * sits at 100 FPS / 0% CPU. That is the display doing nothing, not doing
 * brilliantly. The figure only means something while something is moving.
 *
 * Position is the middle of the face: the default, bottom right, is off the
 * edge of a round panel entirely. It sits over the gauge, which is the point -
 * it is a measuring tool, not part of the instrument. */
/* Off for now, on request. The render timing it showed is still on the serial
 * console, from Panel_RenderTotalUs() and friends, which is the better
 * instrument anyway - it averages over hundreds of frames. Turn both back on
 * to see it on the glass. */
#define LV_USE_SYSMON		0
#define LV_USE_PERF_MONITOR	0
#define LV_USE_PERF_MONITOR_POS	LV_ALIGN_CENTER
#define LV_USE_MEM_MONITOR	0
#define LV_USE_MEM_MONITOR_POS	LV_ALIGN_BOTTOM_MID

/*---------------------------------------------------------------------------*/
/* Fonts                                                                     */
/*---------------------------------------------------------------------------*/

/* A 2 inch gauge read at arm's length across a dashboard needs large digits,
 * so the value text gets 48 px and the signal name 14. Fonts cost flash, not
 * RAM, and there is 4 MB of it. */
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
#define LV_USE_ARCLABEL		0
#define LV_USE_CALENDAR		0
#define LV_USE_CANVAS		0
#define LV_USE_CHECKBOX		0
#define LV_USE_DROPDOWN		0
#define LV_USE_IMAGEBUTTON	0
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
