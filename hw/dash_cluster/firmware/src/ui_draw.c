/*
 * ui_draw.c - the paletted back buffer. See ui_draw.h.
 */

#include "ui_draw.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

/* 212 KB, and the largest single object in the firmware by a wide margin. It
   only fits because LVGL is not linked into this build: its 128 KB heap and
   the 91 KB partial-render buffer together are more than this costs. */
static uint8_t		BackBuf[UI_DRAW_PIXELS];

/* RGB565 in the PANEL'S byte order, high byte first - the same order the faces
   are stored in and the order the panel wants on the wire.

   Each entry is built as (LowAddressByte | HighAddressByte << 8) so that
   storing it to memory little-endian reproduces the two bytes in the order
   they were read. Nothing here needs to know which half is red: the value is
   carried through unexamined, which is also why palettising is lossless. */
static uint16_t		Palette[UI_DRAW_PALETTE_MAX];
static uint16_t		PaletteUsed;
static bool		PaletteOverflow;
static uint32_t		LoadUs;

/* The last index matched. Runs of one colour are long on a dial - the face is
   mostly background - so this turns the linear search below into a single
   compare for the great majority of pixels. */
static uint16_t		LastIndex;


/***************************************************************************************/
/* The palette index for a pixel, adding it to the table if it is new.
 *
 * A linear search rather than a hash. It is O(palette) in the worst case, but
 * it runs ONCE PER FACE AT BOOT, not per frame, and the cache above collapses
 * the common case to one compare; measured cost is reported by UiDraw_LoadUs()
 * rather than reasoned about. A hash table would need 4 KB of SRAM in a build
 * whose whole point is that SRAM is scarce.
 */
static uint8_t UiDraw_Index(uint16_t Pixel)
{
	uint16_t i;

	if (Palette[LastIndex] == Pixel)
		return (uint8_t)LastIndex;

	for (i = 0; i < PaletteUsed; i++)
	{
		if (Palette[i] == Pixel)
		{
			LastIndex = i;
			return (uint8_t)i;
		}
	}

	if (PaletteUsed >= UI_DRAW_PALETTE_MAX)
	{
		/* Out of table. Returning black would silently redraw part of the
		   face in the wrong colour, so the flag is what the caller reports;
		   this return value is only there to keep the write in bounds. */
		PaletteOverflow = true;
		return 0;
	}

	Palette[PaletteUsed] = Pixel;
	LastIndex = PaletteUsed;
	PaletteUsed++;
	return (uint8_t)LastIndex;
}


/***************************************************************************************/
void UiDraw_Init(void)
{
	memset(BackBuf, 0, sizeof(BackBuf));

	/* Index 0 is black, claimed before any face is seen, so that a cleared
	   buffer means a black screen. */
	Palette[0] = 0x0000u;
	PaletteUsed = 1u;
	LastIndex = 0u;
	PaletteOverflow = false;
}


/***************************************************************************************/
bool UiDraw_LoadFace(const DashFace_t *Face, int32_t X, int32_t Y)
{
	const uint8_t *Src;
	uint32_t Stride;
	uint32_t Start;
	int32_t Row;

	if (Face == NULL || Face->Image == NULL)
		return false;

	/* Refused rather than clipped. A face that does not fit is a geometry
	   mistake upstream, and drawing most of it would hide that. */
	if (X < 0 || Y < 0
	    || (X + Face->Width) > UI_DRAW_WIDTH
	    || (Y + Face->Height) > UI_DRAW_HEIGHT)
		return false;

	Src = Face->Image->Data;
	Stride = Face->Image->Stride;
	Start = time_us_32();

	for (Row = 0; Row < Face->Height; Row++)
	{
		const uint8_t *S = Src + ((uint32_t)Row * Stride);
		uint8_t *D = &BackBuf[((uint32_t)(Y + Row) * (uint32_t)UI_DRAW_WIDTH)
		                      + (uint32_t)X];
		int32_t Col;

		for (Col = 0; Col < Face->Width; Col++)
		{
			uint16_t Pixel = (uint16_t)((uint16_t)S[0]
			                            | ((uint16_t)S[1] << 8));

			S += 2;
			*D++ = UiDraw_Index(Pixel);
		}
	}

	LoadUs = (uint32_t)(time_us_32() - Start);
	return !PaletteOverflow;
}


/***************************************************************************************/
uint8_t *UiDraw_Buffer(void)		{ return BackBuf; }
const uint16_t *UiDraw_Palette(void)	{ return Palette; }
uint16_t UiDraw_PaletteUsed(void)	{ return PaletteUsed; }
bool UiDraw_PaletteFull(void)		{ return PaletteOverflow; }
uint32_t UiDraw_LoadUs(void)		{ return LoadUs; }
