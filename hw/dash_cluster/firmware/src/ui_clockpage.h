/*
 * ui_clockpage.h
 *
 * The clock page: the time kept from the RTC and whatever the bus announces,
 * and the hands drawn over the pre-rendered face.
 *
 * The time is one for the node, advanced every frame so the second hand sweeps
 * rather than steps, and re-read from the RTC often enough that it cannot
 * drift away from it. The view - what has been drawn where - belongs to a
 * page, since during a swipe a page may be drawn into either surface.
 */
#ifndef UI_CLOCKPAGE_H_
#define UI_CLOCKPAGE_H_

#include <stdbool.h>
#include <stdint.h>

#include "rtc.h"
#include "ui_needle.h"

/* Three hands and the hub. */
#define UI_CLOCKPAGE_SHAPES	(4u)

/* How often the RTC itself is read. Between reads the time is advanced from
   the frame clock, which is what makes the second hand sweep smoothly; this
   is what stops that drifting from the clock it is meant to be showing. */
#define UI_CLOCKPAGE_READ_MS	(500u)

typedef void (*UiClockDirty_t)(void *Context, const UiRect_t *Rect);

typedef struct
{
	uint8_t Surface;
	float Cx, Cy;
	int32_t Radius;

	uint32_t ShapeCount;
	UiNeedle_t Shapes[UI_CLOCKPAGE_SHAPES];
	bool Red[UI_CLOCKPAGE_SHAPES];	/* the second hand is red, the rest white */

	bool HaveText;
	char Text[8];
	UiRect_t TextRect;
} UiClockPage_t;

extern void UiClockPage_Init(void);

/* Advance the time, take anything the bus has announced, and keep in step with
   the RTC. Once a frame, whichever page is showing. */
extern void UiClockPage_Sample(uint32_t NowMs, uint32_t FrameUs);

/* A view of the clock page, in a surface whose face is already loaded. */
extern void UiClockPage_Load(UiClockPage_t *View, uint8_t Surface, float Cx, float Cy,
                             int32_t Radius);

/* Redraw whatever has moved, reporting every rectangle touched. */
extern void UiClockPage_Update(UiClockPage_t *View, bool TextDue, UiClockDirty_t Dirty,
                               void *Context);

/* For the console: the time as it stands, and whether it can be trusted. */
extern bool UiClockPage_Time(RtcTime_t *Out);

#endif /* UI_CLOCKPAGE_H_ */
