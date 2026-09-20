/*
 * ui_splash.c - the power-on splash on the glass. See ui_splash.h.
 */

#include "ui_splash.h"

#include <string.h>

#include "pico/stdlib.h"

#include "dash_splash.h"
#include "pages.h"
#include "panel.h"
#include "splash_seq.h"
#include "ui_draw.h"

/* The palette sent with each frame, rebuilt at each frame's brightness. In the
   panel's byte order, like every palette handed to Panel_PushPaletted(). */
static uint16_t Wire[256];


/***************************************************************************************/
/* This node's letter. A fourth identity has none and goes straight from the
   emblem to its gauges. */
static const DashSplashImage_t *UiSplash_Letter(uint8_t NodeId)
{
	switch (NodeId)
	{
	case 0u:	return &DashSplashLetterM;
	case 1u:	return &DashSplashLetterR;
	case 2u:	return &DashSplashLetter2;
	default:	return NULL;
	}
}


/***************************************************************************************/
/* The image's palette at a brightness, as panel words. Entries past the image's
   own are black, which is also what an unused index shows. */
static void UiSplash_Palette(const DashSplashImage_t *Img, uint32_t Level)
{
	uint32_t i;

	memset(Wire, 0, sizeof(Wire));
	for (i = 0; i < Img->PaletteCount && i < 256u; i++)
	{
		uint32_t R = ((uint32_t)Img->Palette[3u * i] * Level) / SPLASH_FULL;
		uint32_t G = ((uint32_t)Img->Palette[(3u * i) + 1u] * Level) / SPLASH_FULL;
		uint32_t B = ((uint32_t)Img->Palette[(3u * i) + 2u] * Level) / SPLASH_FULL;
		uint32_t V = ((R >> 3) << 11) | ((G >> 2) << 5) | (B >> 3);

		/* High byte first on the wire, so stored swapped. */
		Wire[i] = (uint16_t)(((V & 0xFFu) << 8) | (V >> 8));
	}
}


/***************************************************************************************/
/* An image into the buffer, on black. */
static void UiSplash_Draw(uint8_t Surface, const DashSplashImage_t *Img)
{
	uint8_t *Buf = UiDraw_Buffer(Surface);
	int32_t y;

	UiDraw_Clear(Surface);
	for (y = 0; y < Img->Height; y++)
		memcpy(Buf + ((uint32_t)(Img->Y + y) * (uint32_t)UI_DRAW_WIDTH) + (uint32_t)Img->X,
		       Img->Data + ((uint32_t)y * (uint32_t)Img->Width), (size_t)Img->Width);
}


/***************************************************************************************/
/* The whole screen black - between images, so the last glimmer of the one
   fading out cannot stay on the glass under the next. */
static void UiSplash_Black(uint8_t Surface)
{
	memset(Wire, 0, sizeof(Wire));
	UiDraw_Clear(Surface);
	Panel_PushPaletted(UiDraw_Buffer(Surface), (uint32_t)UI_DRAW_WIDTH, Wire,
	                   0, 0, UI_DRAW_WIDTH - 1, UI_DRAW_HEIGHT - 1);
}


/***************************************************************************************/
bool UiSplash_Run(uint8_t Surface, uint8_t NodeId)
{
	const DashSplashImage_t *Letter = UiSplash_Letter(NodeId);
	const DashSplashImage_t *Showing = NULL;
	uint32_t StartMs = to_ms_since_boot(get_absolute_time());
	int32_t Tx, Ty;

	UiSplash_Black(Surface);

	for (;;)
	{
		uint32_t NowMs = to_ms_since_boot(get_absolute_time());
		SplashFrame_t F = Splash_At(NowMs - StartMs, Letter != NULL);
		const DashSplashImage_t *Img;

		Panel_Alive(PANEL_STAGE_DRAW);

		if (F.Show == SPLASH_SHOW_DONE)
			break;

		/* A fault takes the screen now, and a touch means the driver wants
		   the gauges now. Either way the splash stops where it is. */
		Panel_TouchService();
		if (Pages_WarningActive(NowMs) || Panel_TouchDown(&Tx, &Ty))
		{
			UiSplash_Black(Surface);
			return false;
		}

		Img = (F.Show == SPLASH_SHOW_EMBLEM) ? &DashSplashEmblem : Letter;
		if (Img != Showing)
		{
			if (Showing != NULL)
				UiSplash_Black(Surface);
			UiSplash_Draw(Surface, Img);
			Showing = Img;
		}

		UiSplash_Palette(Img, F.Level);
		Panel_PushPaletted(UiDraw_Buffer(Surface), (uint32_t)UI_DRAW_WIDTH, Wire,
		                   Img->X, Img->Y, Img->X + Img->Width - 1,
		                   Img->Y + Img->Height - 1);
	}

	UiSplash_Black(Surface);
	return true;
}
