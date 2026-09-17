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
 * is not a quantisation - no pixel changes value. UiDraw_PaletteFull() says so
 * rather than leaving it to be believed: if a face ever needs more, the table
 * fills and the report is loud.
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

#define UI_DRAW_WIDTH		(PANEL_WIDTH)
#define UI_DRAW_HEIGHT		(PANEL_HEIGHT)
#define UI_DRAW_PIXELS		((uint32_t)UI_DRAW_WIDTH * (uint32_t)UI_DRAW_HEIGHT)

/* One byte per pixel, so the table can address every value a byte can hold.
   Index 0 is reserved for black by UiDraw_Init(), so a cleared buffer is a
   black screen rather than whatever colour happened to be seen first. */
#define UI_DRAW_PALETTE_MAX	(256u)

/* Empty the palette and clear the buffer to black. */
extern void UiDraw_Init(void);

/* Copy a pre-rendered face into the back buffer at X,Y, adding its colours to
   the palette as they are met.

   Returns false if it did not fit the buffer or the palette overflowed - in
   which case what landed is wrong, not merely approximate, and the caller
   should say so rather than show it. */
extern bool UiDraw_LoadFace(const DashFace_t *Face, int32_t X, int32_t Y);

/* The buffer and the table, for the panel to convert and send. */
extern uint8_t *UiDraw_Buffer(void);
extern const uint16_t *UiDraw_Palette(void);
extern uint16_t UiDraw_PaletteUsed(void);
extern bool UiDraw_PaletteFull(void);

/* How long the last UiDraw_LoadFace() took, in microseconds. Boot-time cost,
   worth knowing because it is the price of palettising on the node instead of
   on the PC - the step RENDERER_PLAN.md phase 1 moves into face_render. */
extern uint32_t UiDraw_LoadUs(void);

#endif /* UI_DRAW_H_ */
