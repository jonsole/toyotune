/*
 * dash_font.h
 *
 * A bitmap font the firmware draws itself - no LVGL. The fonts are GENERATED
 * by tools/gen_font.py with --format plain; regenerate rather than edit.
 *
 * Glyphs are 4 bpp coverage, high nibble first, packed continuously across a
 * glyph's rows, each glyph starting on a byte boundary - the same packing the
 * LVGL fonts used, so the generator is shared.
 *
 * Glyph placement follows the usual baseline convention: a glyph's box starts
 * X pixels right of the pen, and its BOTTOM edge is Y pixels above the
 * baseline - so its top row is at baseline - (Y + Height).
 */
#ifndef DASH_FONT_H_
#define DASH_FONT_H_

#include <stdint.h>

/* Printable ASCII only, which is all a gauge prints. */
#define DASH_FONT_FIRST_CHAR	(32)
#define DASH_FONT_CHAR_COUNT	(95)

typedef struct
{
	uint32_t Bitmap;		/* offset of this glyph's pixels in the font's bitmap */
	uint8_t Width;
	uint8_t Height;
	int8_t X;			/* left bearing */
	int8_t Y;			/* baseline up to the bottom of the box */
	uint8_t Advance;		/* whole pixels to the next pen position */
} DashGlyph_t;

typedef struct
{
	const uint8_t *Bitmap;
	const DashGlyph_t *Glyphs;
	/* For each printable ASCII code, 1 + its index in Glyphs, or 0 if the font
	   does not carry it. */
	const uint8_t *Map;
	uint8_t LineHeight;
	uint8_t BaseLine;		/* bottom of the line up to the baseline */
	uint8_t DigitHeight;		/* ink height of the digits, for centring a reading */
} DashFont_t;

#endif /* DASH_FONT_H_ */
