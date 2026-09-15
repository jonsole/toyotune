/*
 * face_render.c - render every gauge dial with LVGL on the PC.
 *
 * Built and run by build_faces.py; not part of the firmware. For each gauge in
 * Pages[] this builds the dial with UiGauge_CreateScale() on a screen set up
 * exactly as the firmware sets one up, snapshots the whole panel, crops the
 * gauge's own rectangle out of it, and writes it byte-swapped - the CO5300 and
 * the firmware's draw buffer are both RGB565 high byte first, so the image
 * blits without a per-pixel conversion.
 *
 *   face_render <outdir>
 *
 * writes <outdir>/p<page>_e<element>.bin for each gauge and <outdir>/faces.txt,
 * one "page element width height" line each, which build_faces.py turns into
 * src/dash_faces.c.
 *
 * Snapshotting the screen rather than the scale object matters: an object's
 * snapshot includes its extended draw area and so is not the element's size,
 * while a crop of the screen is exactly the rectangle the firmware places the
 * image at, on the same black it will sit on.
 */

#include <stdio.h>
#include <stdlib.h>

#include "lvgl.h"

#include "pages.h"
#include "panel.h"
#include "ui_gauge.h"

static uint8_t DrawBuf[PANEL_WIDTH * PANEL_HEIGHT * 2];

static void Flush(lv_display_t *Display, const lv_area_t *Area, uint8_t *Pixels)
{
	(void)Area;
	(void)Pixels;
	lv_display_flush_ready(Display);
}

/* The same screen UiLvgl_MakeScreen() builds, less its gesture handler. */
static lv_obj_t *MakeScreen(void)
{
	lv_obj_t *New = lv_obj_create(NULL);

	lv_obj_set_style_bg_color(New, lv_color_black(), LV_PART_MAIN);
	lv_obj_set_style_bg_opa(New, LV_OPA_COVER, LV_PART_MAIN);
	lv_obj_set_style_border_width(New, 0, LV_PART_MAIN);
	lv_obj_set_style_pad_all(New, 0, LV_PART_MAIN);
	lv_obj_set_style_radius(New, 0, LV_PART_MAIN);
	lv_obj_remove_flag(New, LV_OBJ_FLAG_SCROLLABLE);
	return New;
}

int main(int argc, char **argv)
{
	lv_display_t *Display;
	FILE *Manifest;
	char Path[512];
	uint8_t p, e;
	unsigned Written = 0;

	if (argc != 2)
	{
		fprintf(stderr, "usage: face_render <outdir>\n");
		return 2;
	}

	lv_init();

	Display = lv_display_create(PANEL_WIDTH, PANEL_HEIGHT);
	lv_display_set_color_format(Display, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(Display, DrawBuf, NULL, sizeof(DrawBuf),
	                       LV_DISPLAY_RENDER_MODE_PARTIAL);
	lv_display_set_flush_cb(Display, Flush);

	snprintf(Path, sizeof(Path), "%s/faces.txt", argv[1]);
	Manifest = fopen(Path, "w");
	if (Manifest == NULL)
	{
		fprintf(stderr, "cannot write %s\n", Path);
		return 1;
	}

	for (p = 0; p < PageCount; p++)
	{
		for (e = 0; e < Pages[p].ElementCount; e++)
		{
			const FaceElement_t *El = &Pages[p].Elements[e];
			int32_t X, Y, W, H, Row, Col;
			lv_obj_t *Screen;
			lv_draw_buf_t *Snap;
			FILE *Out;

			if (El->Type != WIDGET_GAUGE)
				continue;

			/* From the display resolution, as UiLvgl_BuildPage() does. */
			X = UiGauge_Pct(El->X, PANEL_WIDTH);
			Y = UiGauge_Pct(El->Y, PANEL_HEIGHT);
			W = UiGauge_Pct(El->W, PANEL_WIDTH);
			H = UiGauge_Pct(El->H, PANEL_HEIGHT);

			Screen = MakeScreen();
			UiGauge_CreateScale(Screen, El, X, Y, W, H);
			lv_screen_load(Screen);
			lv_obj_update_layout(Screen);

			Snap = lv_snapshot_take(Screen, LV_COLOR_FORMAT_RGB565);
			if (Snap == NULL
			    || Snap->header.w != PANEL_WIDTH || Snap->header.h != PANEL_HEIGHT)
			{
				fprintf(stderr, "page %u element %u: snapshot failed\n", p, e);
				return 1;
			}

			snprintf(Path, sizeof(Path), "%s/p%u_e%u.bin", argv[1], p, e);
			Out = fopen(Path, "wb");
			if (Out == NULL)
			{
				fprintf(stderr, "cannot write %s\n", Path);
				return 1;
			}

			for (Row = Y; Row < Y + H; Row++)
			{
				const uint8_t *Line = Snap->data + (uint32_t)Row * Snap->header.stride;

				for (Col = X; Col < X + W; Col++)
				{
					/* Native RGB565 is low byte first; the panel wants high
					   byte first. */
					uint8_t Lo = Line[Col * 2];
					uint8_t Hi = Line[Col * 2 + 1];

					fputc(Hi, Out);
					fputc(Lo, Out);
				}
			}
			fclose(Out);

			fprintf(Manifest, "%u %u %d %d\n", p, e, (int)W, (int)H);
			printf("page %u element %u: %dx%d at %d,%d\n", p, e,
			       (int)W, (int)H, (int)X, (int)Y);

			lv_draw_buf_destroy(Snap);
			Written++;
		}
	}

	fclose(Manifest);
	printf("%u face(s) rendered\n", Written);
	return Written ? 0 : 1;
}
