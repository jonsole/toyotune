/*
 * lv_conf.h for the face renderer - the firmware's own configuration, with the
 * few things that cannot apply on a PC overridden.
 *
 * Deliberately the SAME lv_conf.h underneath, not a copy: the dial is only a
 * faithful picture of what the board would draw if the fonts, colour depth,
 * theme and every drawing option match exactly.
 */
#ifndef DASH_FACE_RENDER_LV_CONF_H
#define DASH_FACE_RENDER_LV_CONF_H

#include "../../lv_conf.h"

/* The firmware puts LVGL's hot paths in the RP2350's SRAM section. A GCC
   attribute naming a Pico linker section means nothing to MSVC. */
#undef LV_ATTRIBUTE_FAST_MEM
#define LV_ATTRIBUTE_FAST_MEM

/* The renderer's whole job. */
#undef LV_USE_SNAPSHOT
#define LV_USE_SNAPSHOT		1

/* A snapshot is taken at four times the panel's size in RGB888 - 10 MB -
   and a few layers of that size can be alive at once. The C library's
   allocator rather than a fixed pool: sizing a pool for that is guesswork, and
   a PC has the memory. */
#undef LV_USE_STDLIB_MALLOC
#define LV_USE_STDLIB_MALLOC	LV_STDLIB_CLIB

/* The snapshot is RGB888, so the software renderer must be able to draw it. */
#undef LV_DRAW_SW_SUPPORT_RGB888
#define LV_DRAW_SW_SUPPORT_RGB888	1

#endif /* DASH_FACE_RENDER_LV_CONF_H */
