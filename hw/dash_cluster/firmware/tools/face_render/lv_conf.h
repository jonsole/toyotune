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

/* A snapshot of the full panel is a 434 KB draw buffer, allocated from LVGL's
   own heap - far past the firmware's 128 KB, and free on a PC. */
#undef LV_MEM_SIZE
#define LV_MEM_SIZE		(16U * 1024U * 1024U)

#endif /* DASH_FACE_RENDER_LV_CONF_H */
