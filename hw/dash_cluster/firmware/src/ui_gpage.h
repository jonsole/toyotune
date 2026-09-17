/*
 * ui_gpage.h
 *
 * The g-force page: the accelerometer read once a frame into the g-meter's
 * model, and the model drawn - dot, fading trail, held peaks, two readings -
 * over the pre-rendered friction circle.
 *
 * The model is one for the node, updated every frame whichever page is on the
 * glass, so peaks keep being collected while another page is showing. The
 * view - what has been drawn where - belongs to a page, since during a swipe
 * a page may be drawn into either surface.
 */
#ifndef UI_GPAGE_H_
#define UI_GPAGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "ui_gmeter.h"
#include "ui_needle.h"

/* Dot, trail and four peak marks. */
#define UI_GPAGE_SHAPES		(GMETER_TRAIL_POINTS + 5u)

typedef void (*UiGPageDirty_t)(void *Context, const UiRect_t *Rect);

typedef struct
{
	uint8_t Surface;
	float Cx, Cy;			/* the dial's centre */

	/* What is on the glass now, so it can be put back. */
	uint32_t ShapeCount;
	UiNeedle_t Shapes[UI_GPAGE_SHAPES];
	uint8_t Levels[UI_GPAGE_SHAPES];

	bool HaveText[2];		/* 0 longitudinal, above; 1 lateral, below */
	char Text[2][16];
	UiRect_t TextRect[2];
} UiGPage_t;

/* Once, on core 1, after Imu_Init(). */
extern void UiGPage_Init(void);

/* Read the accelerometer and advance the model. Once a frame, whichever page
   is showing. */
extern void UiGPage_Sample(uint32_t FrameUs);

/* A view of the page, in a surface whose face is already loaded, centred on
   the dial. Draws everything. */
extern void UiGPage_Load(UiGPage_t *Page, uint8_t Surface, float Cx, float Cy);

/* Redraw what has changed since the last call, telling Dirty about every
   rectangle touched. */
extern void UiGPage_Update(UiGPage_t *Page, bool TextDue, UiGPageDirty_t Dirty,
                           void *Context);

/* A tap: clear the peaks. */
extern void UiGPage_ResetPeaks(void);

/* A long press: take the present reading as level. False if the reading is
   not one of a car standing still - see GMeter_Zero(). */
extern bool UiGPage_Zero(void);

/* For the console. */
extern const GMeter_t *UiGPage_Model(void);
extern const GMeterCal_t *UiGPage_Calibration(void);

#endif /* UI_GPAGE_H_ */
