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

#define UI_DRAW_WIDTH		(PANEL_WIDTH)
#define UI_DRAW_HEIGHT		(PANEL_HEIGHT)
#define UI_DRAW_PIXELS		((uint32_t)UI_DRAW_WIDTH * (uint32_t)UI_DRAW_HEIGHT)

/* One byte per pixel, so the table can address every value a byte can hold.
   Index 0 is black, so a cleared buffer is a black screen. */
#define UI_DRAW_PALETTE_MAX	(DASH_FACE_PALETTE_SIZE)

/* Clear the buffer to black, load the faces' palette and add the live
   colours - the needle's - after it. */
extern void UiDraw_Init(void);

/* Copy a pre-rendered face into the back buffer at X,Y.

   Returns false, having copied nothing, if it would not fit - a geometry
   mistake upstream that the caller should report rather than show. */
extern bool UiDraw_LoadFace(const DashFace_t *Face, int32_t X, int32_t Y);

/* Put the face back over an area - under where a needle was, typically. Black
   wherever the area lies outside the face. Clipped to the screen. */
extern void UiDraw_Restore(const UiRect_t *Area);

/* Draw a needle into the buffer, hard-edged, in the needle's colour. Returns
   the pixels set. */
extern uint32_t UiDraw_Needle(const UiNeedle_t *Needle);

/* The buffer and the table, for the panel to convert and send. */
extern uint8_t *UiDraw_Buffer(void);
extern const uint16_t *UiDraw_Palette(void);
extern uint16_t UiDraw_PaletteUsed(void);

/* How long the last UiDraw_LoadFace() took, in microseconds: a copy of the
   face out of XIP flash, and so a direct reading of what flash costs to read
   here, which the needle and text will have to live with too. */
extern uint32_t UiDraw_LoadUs(void);

#endif /* UI_DRAW_H_ */
