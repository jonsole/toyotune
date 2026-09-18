/*
 * face_render.c - render every gauge dial with LVGL on the PC.
 *
 * Built and run by build_faces.py; not part of the firmware. For each page in
 * Pages[] that has gauges, this builds the face with UiGauge_CreateFace() and
 * each gauge with UiGauge_CreateScale() on a black screen
 * UI_GAUGE_RENDER_SCALE times the panel's size, snapshots it in RGB888, and
 * crops out the gauges' rectangle - one picture per page, since a page's
 * gauges share a face. RGB888 rather than the panel's RGB565
 * because the design colours must come through exactly: build_faces.py
 * recognises them by value when it reduces the image to the panel's size.
 *
 *   face_render <outdir>
 *
 * writes <outdir>/p<page>_e<element>.rgb (R, G, B per render pixel) for each
 * page, named after its first gauge, and <outdir>/faces.txt, one
 * "page element width height scale" line each - width and height in PANEL
 * pixels - which build_faces.py turns into src/dash_faces.c.
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
#include "ui_gauge_scale.h"

#define RENDER_WIDTH	(PANEL_WIDTH * UI_GAUGE_RENDER_SCALE)
#define RENDER_HEIGHT	(PANEL_HEIGHT * UI_GAUGE_RENDER_SCALE)

/* The display's own buffer is never looked at - the snapshot renders into one
   of its own - but LVGL wants one to exist. A band of rows is plenty. */
static uint8_t DrawBuf[RENDER_WIDTH * 64 * 2];

static void Flush(lv_display_t *Display, const lv_area_t *Area, uint8_t *Pixels)
{
	(void)Area;
	(void)Pixels;
	lv_display_flush_ready(Display);
}

/* A black, borderless, unscrollable screen - what the firmware draws on. */
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

	Display = lv_display_create(RENDER_WIDTH, RENDER_HEIGHT);
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
		const FaceElement_t *First = NULL;
		uint8_t FirstIndex = 0;
		bool Split = false;
		bool GMeter = false;
		bool Clock = false;
		const FaceElement_t *Graphs[2];
		uint32_t GraphCount = 0;
		int32_t X, Y, W, H, Row, Col;
		lv_obj_t *Screen;
		lv_draw_buf_t *Snap;
		FILE *Out;

		/* A page's gauges share one face, so they must share one rectangle -
		   a half gauge is half of the same dial, not a dial of its own. */
		for (e = 0; e < Pages[p].ElementCount; e++)
		{
			const FaceElement_t *El = &Pages[p].Elements[e];

			if (El->Type != WIDGET_GAUGE && El->Type != WIDGET_GFORCE
			    && El->Type != WIDGET_GRAPH && El->Type != WIDGET_CLOCK)
				continue;
			if (El->Type == WIDGET_CLOCK)
				Clock = true;
			if (El->Type == WIDGET_GFORCE)
				GMeter = true;
			if (El->Type == WIDGET_GRAPH && GraphCount < 2u)
				Graphs[GraphCount++] = El;
			if (First == NULL)
			{
				First = El;
				FirstIndex = e;
			}
			else if (El->X != First->X || El->Y != First->Y
			         || El->W != First->W || El->H != First->H)
			{
				fprintf(stderr, "page %u: gauges %u and %u do not share a rectangle\n",
				        p, FirstIndex, e);
				return 1;
			}
			/* A split face is one with a top and a bottom half sharing it -
			   which is about the HALF sweeps, not about any sweep that is not
			   the full dial. A clock is a full circle and had been getting the
			   divider drawn across it. */
			if (El->Sweep == GAUGE_SWEEP_TOP || El->Sweep == GAUGE_SWEEP_BOTTOM)
				Split = true;
		}
		if (First == NULL)
			continue;

		/* In PANEL pixels, from the panel's resolution, then scaled - so the
		   render rectangle is an exact multiple and every panel pixel is a
		   whole block of render pixels. */
		X = UiGauge_Pct(First->X, PANEL_WIDTH);
		Y = UiGauge_Pct(First->Y, PANEL_HEIGHT);
		W = UiGauge_Pct(First->W, PANEL_WIDTH);
		H = UiGauge_Pct(First->H, PANEL_HEIGHT);

		Screen = MakeScreen();
		UiGauge_CreateFace(Screen,
		                   X * UI_GAUGE_RENDER_SCALE, Y * UI_GAUGE_RENDER_SCALE,
		                   W * UI_GAUGE_RENDER_SCALE, H * UI_GAUGE_RENDER_SCALE, Split,
		                   !GMeter && !Clock && GraphCount == 0u);
		for (e = 0; e < Pages[p].ElementCount; e++)
		{
			if (Pages[p].Elements[e].Type == WIDGET_GAUGE
			    || Pages[p].Elements[e].Type == WIDGET_CLOCK)
				UiGauge_CreateScale(Screen, &Pages[p].Elements[e],
				                    X * UI_GAUGE_RENDER_SCALE, Y * UI_GAUGE_RENDER_SCALE,
				                    W * UI_GAUGE_RENDER_SCALE, H * UI_GAUGE_RENDER_SCALE);
			else if (Pages[p].Elements[e].Type == WIDGET_GFORCE)
				UiGauge_CreateGMeter(Screen,
				                     X * UI_GAUGE_RENDER_SCALE, Y * UI_GAUGE_RENDER_SCALE,
				                     W * UI_GAUGE_RENDER_SCALE, H * UI_GAUGE_RENDER_SCALE);
		}
		if (GraphCount != 0u)
			UiGauge_CreateGraph(Screen, Graphs, GraphCount,
			                    X * UI_GAUGE_RENDER_SCALE, Y * UI_GAUGE_RENDER_SCALE,
			                    W * UI_GAUGE_RENDER_SCALE, H * UI_GAUGE_RENDER_SCALE);
		lv_screen_load(Screen);
		lv_obj_update_layout(Screen);

		Snap = lv_snapshot_take(Screen, LV_COLOR_FORMAT_RGB888);
		if (Snap == NULL
		    || Snap->header.w != RENDER_WIDTH || Snap->header.h != RENDER_HEIGHT)
		{
			fprintf(stderr, "page %u: snapshot failed\n", p);
			return 1;
		}

		snprintf(Path, sizeof(Path), "%s/p%u_e%u.rgb", argv[1], p, FirstIndex);
		Out = fopen(Path, "wb");
		if (Out == NULL)
		{
			fprintf(stderr, "cannot write %s\n", Path);
			return 1;
		}

		for (Row = Y * UI_GAUGE_RENDER_SCALE;
		     Row < (Y + H) * UI_GAUGE_RENDER_SCALE; Row++)
		{
			const uint8_t *Line = Snap->data + (uint32_t)Row * Snap->header.stride;

			for (Col = X * UI_GAUGE_RENDER_SCALE;
			     Col < (X + W) * UI_GAUGE_RENDER_SCALE; Col++)
			{
				/* LVGL's RGB888 is stored B, G, R. */
				fputc(Line[Col * 3 + 2], Out);
				fputc(Line[Col * 3 + 1], Out);
				fputc(Line[Col * 3], Out);
			}
		}
		fclose(Out);

		fprintf(Manifest, "%u %u %d %d %d\n", p, FirstIndex, (int)W, (int)H,
		        UI_GAUGE_RENDER_SCALE);
		printf("page %u: %dx%d at %d,%d, %s, rendered at %dx\n",
		       p, (int)W, (int)H, (int)X, (int)Y,
		       GMeter ? "g-force"
		              : (GraphCount ? "trace"
		                            : (Clock ? "clock" : (Split ? "split" : "one gauge"))),
		       UI_GAUGE_RENDER_SCALE);

		lv_draw_buf_destroy(Snap);
		Written++;
	}

	fclose(Manifest);
	printf("%u face(s) rendered\n", Written);
	return Written ? 0 : 1;
}
