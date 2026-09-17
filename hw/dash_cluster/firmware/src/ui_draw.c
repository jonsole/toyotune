/*
 * ui_draw.c - the paletted back buffer. See ui_draw.h.
 */

#include "ui_draw.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ui_gauge.h"

/* 212 KB, and the largest single object in the firmware by a wide margin. It
   only fits because LVGL is not linked into this build: its 128 KB heap and
   the 91 KB partial-render buffer together cost more than this does. */
static uint8_t		BackBuf[UI_DRAW_PIXELS];

/* A copy of DashFacePalette in SRAM. The conversion loop reads it for every
   pixel of every frame, and 512 bytes is a small price for keeping that read
   off XIP flash - and it is where the colours drawn live are added, in the
   entries the faces left free. */
static uint16_t		Palette[UI_DRAW_PALETTE_MAX];
static uint16_t		PaletteUsed;
static uint8_t		NeedleIndex;
static uint32_t		LoadUs;

/* The face in the buffer, and where, so what a needle covered can be put back. */
static const DashFace_t	*Face;
static int32_t		FaceX;
static int32_t		FaceY;


/***************************************************************************************/
/* An 0xRRGGBB colour as a palette entry: RGB565, stored so that it lands in
   memory high byte first, as the faces' entries do - see dash_faces.h. */
static uint16_t UiDraw_Entry(uint32_t Rgb)
{
	uint32_t R = (Rgb >> 16) & 0xFFu;
	uint32_t G = (Rgb >> 8) & 0xFFu;
	uint32_t B = Rgb & 0xFFu;
	uint32_t V = (((R * 31u + 127u) / 255u) << 11)
	             | (((G * 63u + 127u) / 255u) << 5)
	             | ((B * 31u + 127u) / 255u);

	return (uint16_t)(((V & 0xFFu) << 8) | (V >> 8));
}


/***************************************************************************************/
/* Add a live colour after the faces' own. If they have left no room - which
   would take a redesign; they use 34 of 256 - the colour lands on the last
   entry and that is reported, rather than silently recolouring the face. */
static uint8_t UiDraw_AddColour(uint32_t Rgb)
{
	uint16_t Entry = UiDraw_Entry(Rgb);

	if (PaletteUsed >= UI_DRAW_PALETTE_MAX)
	{
		printf("ui_draw: palette full, live colour 0x%06lx overwrites entry %u\n",
		       (unsigned long)Rgb, (unsigned)(UI_DRAW_PALETTE_MAX - 1u));
		Palette[UI_DRAW_PALETTE_MAX - 1u] = Entry;
		return (uint8_t)(UI_DRAW_PALETTE_MAX - 1u);
	}

	Palette[PaletteUsed] = Entry;
	return (uint8_t)PaletteUsed++;
}


/***************************************************************************************/
void UiDraw_Init(void)
{
	/* Index 0 is black - the generator guarantees it - so a cleared buffer is
	   a black screen. */
	memset(BackBuf, 0, sizeof(BackBuf));
	memcpy(Palette, DashFacePalette, sizeof(Palette));
	PaletteUsed = DashFacePaletteUsed;
	NeedleIndex = UiDraw_AddColour(UI_GAUGE_NEEDLE_COLOUR);
	Face = NULL;
}


/***************************************************************************************/
bool UiDraw_LoadFace(const DashFace_t *NewFace, int32_t X, int32_t Y)
{
	const uint8_t *Src;
	uint32_t Start;
	int32_t Row;

	if (NewFace == NULL || NewFace->Image == NULL)
		return false;

	/* Refused rather than clipped. A face that does not fit is a geometry
	   mistake upstream, and drawing most of it would hide that. */
	if (X < 0 || Y < 0
	    || (X + NewFace->Width) > UI_DRAW_WIDTH
	    || (Y + NewFace->Height) > UI_DRAW_HEIGHT
	    || NewFace->Image->Stride != (uint32_t)NewFace->Width)
		return false;

	Src = NewFace->Image->Data;
	Start = time_us_32();

	/* Already in the buffer's own format, so a face is a copy per row. */
	for (Row = 0; Row < NewFace->Height; Row++)
	{
		memcpy(&BackBuf[((uint32_t)(Y + Row) * (uint32_t)UI_DRAW_WIDTH)
		                + (uint32_t)X],
		       Src + ((uint32_t)Row * NewFace->Image->Stride),
		       (size_t)NewFace->Width);
	}

	Face = NewFace;
	FaceX = X;
	FaceY = Y;
	LoadUs = (uint32_t)(time_us_32() - Start);
	return true;
}


/***************************************************************************************/
void UiDraw_Restore(const UiRect_t *Area)
{
	int32_t X1 = (Area->X1 < 0) ? 0 : Area->X1;
	int32_t Y1 = (Area->Y1 < 0) ? 0 : Area->Y1;
	int32_t X2 = (Area->X2 >= UI_DRAW_WIDTH) ? (UI_DRAW_WIDTH - 1) : Area->X2;
	int32_t Y2 = (Area->Y2 >= UI_DRAW_HEIGHT) ? (UI_DRAW_HEIGHT - 1) : Area->Y2;
	int32_t Y;

	if (X1 > X2 || Y1 > Y2)
		return;		/* wholly off the screen */

	for (Y = Y1; Y <= Y2; Y++)
	{
		uint8_t *Row = &BackBuf[(uint32_t)Y * (uint32_t)UI_DRAW_WIDTH];
		int32_t X = X1;

		/* Everything outside the face is black: before it, after it, and every
		   row above or below it. */
		if (Face == NULL || Y < FaceY || Y >= FaceY + Face->Height)
		{
			memset(&Row[X1], 0, (size_t)(X2 - X1 + 1));
			continue;
		}

		if (X < FaceX)
		{
			int32_t End = (X2 < FaceX - 1) ? X2 : (FaceX - 1);

			memset(&Row[X], 0, (size_t)(End - X + 1));
			X = End + 1;
		}

		if (X <= X2 && X < FaceX + Face->Width)
		{
			int32_t End = (X2 < FaceX + Face->Width - 1) ? X2
			                                             : (FaceX + Face->Width - 1);

			memcpy(&Row[X],
			       Face->Image->Data + ((uint32_t)(Y - FaceY) * Face->Image->Stride)
			       + (uint32_t)(X - FaceX),
			       (size_t)(End - X + 1));
			X = End + 1;
		}

		if (X <= X2)
			memset(&Row[X], 0, (size_t)(X2 - X + 1));
	}
}


/***************************************************************************************/
uint32_t UiDraw_Needle(const UiNeedle_t *Needle)
{
	return UiNeedle_Draw(Needle, BackBuf, (uint32_t)UI_DRAW_WIDTH,
	                     UI_DRAW_WIDTH, UI_DRAW_HEIGHT, NeedleIndex);
}


/***************************************************************************************/
uint8_t *UiDraw_Buffer(void)		{ return BackBuf; }
const uint16_t *UiDraw_Palette(void)	{ return Palette; }
uint16_t UiDraw_PaletteUsed(void)	{ return PaletteUsed; }
uint32_t UiDraw_LoadUs(void)		{ return LoadUs; }
