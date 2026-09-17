/*
 * ui_text.c - text into the paletted buffer. See ui_text.h.
 */

#include "ui_text.h"

#include <math.h>
#include <stddef.h>


/***************************************************************************************/
static const DashGlyph_t *UiText_Glyph(const DashFont_t *Font, char Ch)
{
	int Code = (int)(unsigned char)Ch - DASH_FONT_FIRST_CHAR;

	if (Code < 0 || Code >= DASH_FONT_CHAR_COUNT || Font->Map[Code] == 0u)
		return NULL;
	return &Font->Glyphs[Font->Map[Code] - 1u];
}


/***************************************************************************************/
int32_t UiText_Width(const DashFont_t *Font, const char *Text)
{
	int32_t Width = 0;

	for (; *Text != '\0'; Text++)
	{
		const DashGlyph_t *G = UiText_Glyph(Font, *Text);

		if (G != NULL)
			Width += (int32_t)G->Advance;
	}
	return Width;
}


/***************************************************************************************/
bool UiText_Bounds(const DashFont_t *Font, const char *Text, int32_t X, int32_t Y,
                   UiRect_t *Out)
{
	bool Any = false;
	UiRect_t R = { 0, 0, 0, 0 };

	for (; *Text != '\0'; Text++)
	{
		const DashGlyph_t *G = UiText_Glyph(Font, *Text);

		if (G == NULL)
			continue;

		if (G->Width != 0u && G->Height != 0u)
		{
			UiRect_t B;

			B.X1 = X + G->X;
			B.X2 = B.X1 + (int32_t)G->Width - 1;
			B.Y2 = Y - G->Y - 1;
			B.Y1 = B.Y2 - (int32_t)G->Height + 1;
			R = Any ? UiRect_Union(&R, &B) : B;
			Any = true;
		}
		X += (int32_t)G->Advance;
	}

	if (Any)
		*Out = R;
	return Any;
}


/***************************************************************************************/
uint32_t UiText_Draw(const DashFont_t *Font, const char *Text, int32_t X, int32_t Y,
                     const uint8_t *Ramp, uint8_t *Buffer, uint32_t Stride,
                     int32_t Width, int32_t Height)
{
	uint32_t Set = 0;

	for (; *Text != '\0'; Text++)
	{
		const DashGlyph_t *G = UiText_Glyph(Font, *Text);
		const uint8_t *Src;
		int32_t Top, Row, Col;
		uint32_t Nibble = 0;

		if (G == NULL)
			continue;

		Src = Font->Bitmap + G->Bitmap;
		Top = Y - G->Y - (int32_t)G->Height;

		/* The pixels run continuously across rows, so every pixel is stepped
		   over, visible or not, to keep the nibble count right. */
		for (Row = 0; Row < (int32_t)G->Height; Row++)
		{
			int32_t Py = Top + Row;

			for (Col = 0; Col < (int32_t)G->Width; Col++, Nibble++)
			{
				int32_t Px = X + G->X + Col;
				uint8_t Byte = Src[Nibble >> 1];
				uint8_t Cover = ((Nibble & 1u) == 0u) ? (uint8_t)(Byte >> 4)
				                                      : (uint8_t)(Byte & 0x0Fu);

				if (Cover == 0u || Px < 0 || Py < 0 || Px >= Width || Py >= Height)
					continue;

				Buffer[((uint32_t)Py * Stride) + (uint32_t)Px] = Ramp[Cover];
				Set++;
			}
		}

		X += (int32_t)G->Advance;
	}

	return Set;
}


/***************************************************************************************/
void UiText_Centre(const DashFont_t *Font, const char *Text, float Cx, float Cy,
                   int32_t *X, int32_t *Y)
{
	/* The digits sit on the baseline, so their ink occupies the DigitHeight
	   rows above it; their middle is at baseline - DigitHeight / 2. */
	*X = (int32_t)floorf(Cx - ((float)UiText_Width(Font, Text) / 2.0f));
	*Y = (int32_t)floorf(Cy + ((float)Font->DigitHeight / 2.0f));
}
