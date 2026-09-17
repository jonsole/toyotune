/*
 * ui_draw.h
 *
 * The dash node's own renderer: an 8-bit paletted back buffer the size of the
 * screen, converted to RGB565 a few lines at a time on its way to the panel.
 *
 * WHY PALETTED, AND WHY 8 BIT
 *
 * A 466x466 RGB565 framebuffer is 434 KB and does not fit in the RP2350's
 * 520 KB beside anything else. At 8 bits it is 212 KB and does, which is what
 * makes a full back buffer possible at all - and a back buffer is what lets
 * the renderer treat the screen as memory rather than as a sequence of
 * rectangles handed to a toolkit.
 *
 * The faces cost nothing to palette: both dials together use 151 distinct
 * colours, anti-aliased edges included, so a 256-entry table is LOSSLESS. It
 * is not a quantisation - no pixel changes value. The palettising is done on
 * the PC by tools/face_render/build_faces.py, which checks the round trip and
 * refuses rather than quantises if the faces ever need more; see
 * dash_faces.h. So a face arrives here already in the buffer's format.
 *
 * The cost is one lookup per pixel on the way out, and that is paid where
 * there is time for it - see Panel_PushPaletted(), which converts the next
 * chunk of lines while the previous one is still being clocked to the glass.
 *
 * See hw/dash_cluster/RENDERER_PLAN.md for where this is going.
 */
#ifndef UI_DRAW_H_
#define UI_DRAW_H_

#include <stdbool.h>
#include <stdint.h>

#include "dash_faces.h"
#include "panel.h"
#include "ui_needle.h"
#include "ui_text.h"

#define UI_DRAW_WIDTH		(PANEL_WIDTH)
#define UI_DRAW_HEIGHT		(PANEL_HEIGHT)
#define UI_DRAW_PIXELS		((uint32_t)UI_DRAW_WIDTH * (uint32_t)UI_DRAW_HEIGHT)

/* One byte per pixel, so the table can address every value a byte can hold.
   Index 0 is black, so a cleared buffer is a black screen. */
#define UI_DRAW_PALETTE_MAX	(DASH_FACE_PALETTE_SIZE)

/* SURFACES. There are two screen-sized buffers: the page on the glass, and
   the page a swipe is bringing in, which is drawn - live - beside it while the
   finger moves. When the swipe completes the two simply change roles. Every
   drawing call names the surface it draws into; they share one palette.

   Two is 424 KB of the RP2350's 512 KB SRAM, which is only possible because
   nothing else here is large. */
#define UI_DRAW_SURFACES	(2u)

/* Load the faces' palette and add the live colours after it - the needle's,
   and the ramp text is drawn with - and clear every surface to black. */
extern void UiDraw_Init(void);

/* Clear one surface to black, forgetting its face. */
extern void UiDraw_Clear(uint8_t Surface);

/* Copy a pre-rendered face into a surface at X,Y.

   Returns false, having copied nothing, if it would not fit - a geometry
   mistake upstream that the caller should report rather than show. */
extern bool UiDraw_LoadFace(uint8_t Surface, const DashFace_t *Face, int32_t X, int32_t Y);

/* Put the surface's face back over an area - under where a needle was,
   typically. Black wherever the area lies outside the face. Clipped to the
   screen. */
extern void UiDraw_Restore(uint8_t Surface, const UiRect_t *Area);

/* Draw a needle, hard-edged, in the needle's colour. Returns the pixels set. */
extern uint32_t UiDraw_Needle(uint8_t Surface, const UiNeedle_t *Needle);

/* THE PALETTE INDICES OF THE DESIGN COLOURS, for code that fills and draws
   lines itself rather than blending - the strip chart's background, grid and
   traces. */
typedef enum
{
	UI_INK_FACE = 0,	/* the dial's charcoal */
	UI_INK_GRID,		/* the ring grey */
	UI_INK_RED,		/* the needle's red */
	UI_INK_WHITE,		/* the markings' warm white */
	UI_INK_COUNT
} UiInk_t;

extern uint8_t UiDraw_Ink(UiInk_t Ink);

/* Draw any round-ended stroke - a needle's shape; with its ends together, a
   dot - in a shade between the face and the needle's red: Level 15 is the red,
   lower levels fade towards the face, 0 draws nothing. Returns the pixels
   set. */
#define UI_DRAW_RED_FULL	(15u)

extern uint32_t UiDraw_Stroke(uint8_t Surface, const UiNeedle_t *Shape, uint8_t Level);

/* Draw text in the markings' colour, pen at X on baseline Y. Anti-aliased by
   index, so correct over the plain face only - see ui_text.h. Returns the
   pixels set. */
extern uint32_t UiDraw_Text(uint8_t Surface, const DashFont_t *Font, const char *Text,
                            int32_t X, int32_t Y);

/* The same, in a chosen colour: the markings' white, or the needle's red for a
   reading that belongs to a red trace. */
typedef enum
{
	UI_TEXT_WHITE = 0,
	UI_TEXT_RED
} UiTextInk_t;

extern uint32_t UiDraw_TextIn(uint8_t Surface, UiTextInk_t Ink, const DashFont_t *Font,
                              const char *Text, int32_t X, int32_t Y);

/* A surface's pixels and the shared table, for the panel to convert and send. */
extern uint8_t *UiDraw_Buffer(uint8_t Surface);
extern const uint16_t *UiDraw_Palette(void);
extern uint16_t UiDraw_PaletteUsed(void);

/* How long the last UiDraw_LoadFace() took, in microseconds: a copy of the
   face out of XIP flash, and so a direct reading of what flash costs to read
   here, which the needle and text will have to live with too. */
extern uint32_t UiDraw_LoadUs(void);

#endif /* UI_DRAW_H_ */
