/*
 * ui_draw.c - the paletted back buffer. See ui_draw.h.
 */

#include "ui_draw.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ui_gauge.h"

/* 212 KB each, and the largest objects in the firmware by a wide margin. They
   only fit because LVGL is not linked into this build: its 128 KB heap and
   91 KB partial-render buffer together cost more than one of these. */
static uint8_t		BackBuf[UI_DRAW_SURFACES][UI_DRAW_PIXELS];

/* A copy of DashFacePalette in SRAM. The conversion loop reads it for every
   pixel of every frame, and 512 bytes is a small price for keeping that read
   off XIP flash - and it is where the colours drawn live are added, in the
   entries the faces left free. */
static uint16_t		Palette[UI_DRAW_PALETTE_MAX];
static uint16_t		PaletteUsed;
static uint8_t		NeedleIndex;

/* Coverage to palette index for text: the markings' colour mixed that far over
   the face - see ui_text.h. */
static uint8_t		TextRamp[UI_TEXT_LEVELS];

/* The face mixed towards the needle's red, for things that fade - the g-force
   trail. Level 15 is the red itself. */
static uint8_t		RedRamp[UI_TEXT_LEVELS];

/* The design colours as plain indices - see UiDraw_Ink(). */
static uint8_t		Inks[UI_INK_COUNT];
static uint32_t		LoadUs;

/* The face in each surface, and where, so what a needle covered can be put
   back. */
typedef struct
{
	const DashFace_t *Face;
	int32_t X;
	int32_t Y;
} UiDrawFace_t;

static UiDrawFace_t	Faces[UI_DRAW_SURFACES];


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
/* UI_TEXT_LEVELS shades from Back to Fore, mixed in the same space the face
   renderer mixes its edge shades in, so text sits on the dial the way the
   printed numerals do. Entry 0 is Back itself and is never drawn. */
static void UiDraw_AddRamp(uint32_t Back, uint32_t Fore, uint8_t *Ramp)
{
	uint32_t Level;

	Ramp[0] = 0u;
	for (Level = 1u; Level < UI_TEXT_LEVELS; Level++)
	{
		uint32_t Mix = 0;
		uint32_t Shift;

		for (Shift = 0; Shift <= 16u; Shift += 8u)
		{
			uint32_t B = (Back >> Shift) & 0xFFu;
			uint32_t F = (Fore >> Shift) & 0xFFu;
			uint32_t Top = (UI_TEXT_LEVELS - 1u);

			Mix |= (((B * (Top - Level)) + (F * Level) + (Top / 2u)) / Top) << Shift;
		}
		Ramp[Level] = UiDraw_AddColour(Mix);
	}
}


/***************************************************************************************/
void UiDraw_Init(void)
{
	uint8_t S;

	memcpy(Palette, DashFacePalette, sizeof(Palette));
	PaletteUsed = DashFacePaletteUsed;
	NeedleIndex = UiDraw_AddColour(UI_GAUGE_NEEDLE_COLOUR);
	UiDraw_AddRamp(UI_GAUGE_FACE_COLOUR, UI_GAUGE_MARK_COLOUR, TextRamp);
	UiDraw_AddRamp(UI_GAUGE_FACE_COLOUR, UI_GAUGE_NEEDLE_COLOUR, RedRamp);
	Inks[UI_INK_FACE] = UiDraw_AddColour(UI_GAUGE_FACE_COLOUR);
	Inks[UI_INK_GRID] = UiDraw_AddColour(UI_GAUGE_RING_COLOUR);
	Inks[UI_INK_RED] = UiDraw_AddColour(UI_GAUGE_NEEDLE_COLOUR);
	Inks[UI_INK_WHITE] = UiDraw_AddColour(UI_GAUGE_MARK_COLOUR);

	for (S = 0; S < UI_DRAW_SURFACES; S++)
		UiDraw_Clear(S);
}


/***************************************************************************************/
void UiDraw_Clear(uint8_t Surface)
{
	/* Index 0 is black - the generator guarantees it - so a cleared buffer is
	   a black screen. */
	memset(BackBuf[Surface], 0, sizeof(BackBuf[Surface]));
	Faces[Surface].Face = NULL;
}


/***************************************************************************************/
bool UiDraw_LoadFace(uint8_t Surface, const DashFace_t *NewFace, int32_t X, int32_t Y)
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
		memcpy(&BackBuf[Surface][((uint32_t)(Y + Row) * (uint32_t)UI_DRAW_WIDTH)
		                         + (uint32_t)X],
		       Src + ((uint32_t)Row * NewFace->Image->Stride),
		       (size_t)NewFace->Width);
	}

	Faces[Surface].Face = NewFace;
	Faces[Surface].X = X;
	Faces[Surface].Y = Y;
	LoadUs = (uint32_t)(time_us_32() - Start);
	return true;
}


/***************************************************************************************/
void UiDraw_Restore(uint8_t Surface, const UiRect_t *Area)
{
	const DashFace_t *Face = Faces[Surface].Face;
	int32_t FaceX = Faces[Surface].X;
	int32_t FaceY = Faces[Surface].Y;
	int32_t X1 = (Area->X1 < 0) ? 0 : Area->X1;
	int32_t Y1 = (Area->Y1 < 0) ? 0 : Area->Y1;
	int32_t X2 = (Area->X2 >= UI_DRAW_WIDTH) ? (UI_DRAW_WIDTH - 1) : Area->X2;
	int32_t Y2 = (Area->Y2 >= UI_DRAW_HEIGHT) ? (UI_DRAW_HEIGHT - 1) : Area->Y2;
	int32_t Y;

	if (X1 > X2 || Y1 > Y2)
		return;		/* wholly off the screen */

	for (Y = Y1; Y <= Y2; Y++)
	{
		uint8_t *Row = &BackBuf[Surface][(uint32_t)Y * (uint32_t)UI_DRAW_WIDTH];
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
uint32_t UiDraw_Needle(uint8_t Surface, const UiNeedle_t *Needle)
{
	return UiNeedle_Draw(Needle, BackBuf[Surface], (uint32_t)UI_DRAW_WIDTH,
	                     UI_DRAW_WIDTH, UI_DRAW_HEIGHT, NeedleIndex);
}


/***************************************************************************************/
uint8_t UiDraw_Ink(UiInk_t Ink)
{
	return (Ink < UI_INK_COUNT) ? Inks[Ink] : 0u;
}


/***************************************************************************************/
uint32_t UiDraw_Stroke(uint8_t Surface, const UiNeedle_t *Shape, uint8_t Level)
{
	if (Level == 0u)
		return 0;
	if (Level >= UI_TEXT_LEVELS)
		Level = (uint8_t)(UI_TEXT_LEVELS - 1u);
	return UiNeedle_Draw(Shape, BackBuf[Surface], (uint32_t)UI_DRAW_WIDTH,
	                     UI_DRAW_WIDTH, UI_DRAW_HEIGHT, RedRamp[Level]);
}


/***************************************************************************************/
uint32_t UiDraw_Text(uint8_t Surface, const DashFont_t *Font, const char *Text,
                     int32_t X, int32_t Y)
{
	return UiDraw_TextIn(Surface, UI_TEXT_WHITE, Font, Text, X, Y);
}


/***************************************************************************************/
uint32_t UiDraw_TextIn(uint8_t Surface, UiTextInk_t Ink, const DashFont_t *Font,
                       const char *Text, int32_t X, int32_t Y)
{
	return UiText_Draw(Font, Text, X, Y, (Ink == UI_TEXT_RED) ? RedRamp : TextRamp,
	                   BackBuf[Surface], (uint32_t)UI_DRAW_WIDTH,
	                   UI_DRAW_WIDTH, UI_DRAW_HEIGHT);
}


/***************************************************************************************/
uint8_t *UiDraw_Buffer(uint8_t Surface)	{ return BackBuf[Surface]; }
const uint16_t *UiDraw_Palette(void)	{ return Palette; }
uint16_t UiDraw_PaletteUsed(void)	{ return PaletteUsed; }
uint32_t UiDraw_LoadUs(void)		{ return LoadUs; }
