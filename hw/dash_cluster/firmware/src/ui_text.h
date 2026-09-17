/*
 * ui_text.h
 *
 * Drawing a line of text into the 8-bit paletted buffer. No hardware and no
 * display dependency, so it is host-tested.
 *
 * BLENDING BY INDEX. Glyphs carry 4 bpp coverage, and a paletted buffer has no
 * colour to blend with - so each coverage level is given its own palette
 * entry, the text colour mixed that far over the background, and a pixel is
 * simply set to the entry for its coverage. That is exact only over the
 * background the ramp was built for, which is why a reading is drawn only
 * inside the dial's centre ring, where the face is one solid colour.
 */
#ifndef UI_TEXT_H_
#define UI_TEXT_H_

#include <stdbool.h>
#include <stdint.h>

#include "dash_font.h"
#include "ui_needle.h"

/* Coverage 0..15 to palette index; entry 0 is never used - no coverage means
   the pixel is left alone. */
#define UI_TEXT_LEVELS	(16u)

/* Pixels from the first pen position to the last: the sum of the advances.
   Characters the font does not carry count as nothing. */
extern int32_t UiText_Width(const DashFont_t *Font, const char *Text);

/* Where the text's ink lies, pen starting at X on baseline Y. Returns false
   for text with no ink at all, when Out is left alone. */
extern bool UiText_Bounds(const DashFont_t *Font, const char *Text, int32_t X,
                          int32_t Y, UiRect_t *Out);

/* Draw it into an 8-bit buffer Width x Height, Stride bytes a row, clipped.
   Returns the pixels set. */
extern uint32_t UiText_Draw(const DashFont_t *Font, const char *Text, int32_t X,
                            int32_t Y, const uint8_t *Ramp, uint8_t *Buffer,
                            uint32_t Stride, int32_t Width, int32_t Height);

/* The pen position that centres the text's advance on Cx, and the baseline
   that centres the font's digits on Cy - continuous coordinates, as for the
   needle, so a 232.5 centre lands between pixels as it should. */
extern void UiText_Centre(const DashFont_t *Font, const char *Text, float Cx,
                          float Cy, int32_t *X, int32_t *Y);

#endif /* UI_TEXT_H_ */
