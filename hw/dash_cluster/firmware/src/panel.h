/*
 * panel.h
 *
 * The CO5300 AMOLED panel and the CST9217 touch controller.
 *
 * This is the only file that talks to the vendor drivers in firmware/vendor/,
 * and the only genuinely board-specific part of the display path. Everything
 * above it - the page tables, what a gauge decides to show, how a value is
 * formatted - is in pages.c, ui_model.c and ui_draw.c and does not know a
 * panel exists.
 *
 * CALL ORDER MATTERS, AND IT SPANS BOTH CORES
 *
 *   core 0, first thing in main():   Panel_ClockInit()
 *   core 1, once:                    Panel_Init()
 *   core 1, once per frame:          Panel_TouchService(), Panel_PushPaletted()
 *
 * Panel_ClockInit() is separate, and runs on core 0 before anything else,
 * because it changes the system clock. Doing that from core 1 while core 0 is
 * running is unsafe, and doing it after can2040 has started would leave the
 * CAN bit timing computed against the old frequency - can_link.c reads
 * clock_get_hz(clk_sys) once, at init.
 */

#ifndef PANEL_H_
#define PANEL_H_

#include <stdbool.h>
#include <stdint.h>

/* Both panel options in PLAN.md section 4.1 are 466x466, which is why the page
   tables are in percent rather than pixels. */
#define PANEL_WIDTH	(466)
#define PANEL_HEIGHT	(466)

/* Set the system and peripheral clocks. Core 0, before stdio and before
   CanLink_Init(). */
extern void Panel_ClockInit(void);

/* Bring up the panel and the touch controller. Core 1 only, once.

   Returns false if the touch controller did not identify itself. The display
   still works in that case and the node still shows gauges - it just cannot
   be swiped - so this is a report, not a reason to stop. */
extern bool Panel_Init(void);

/* HOW FAR Panel_Init() GOT, for core 0 to report.
 *
 * Core 1 brings the panel up and core 1 prints the status line, so anything
 * that wedges during bring-up produces a board that enumerates over USB and
 * says nothing at all - core 0 keeps servicing USB either way. That has been
 * the symptom of every panel fault in this project so far, and it carries no
 * information. This does: core 0 polls the stage and prints it, so a hang
 * names the step it hung on.
 *
 * Written by core 1, read by core 0. A single byte, so it needs no more
 * protection than volatile. */
typedef enum
{
	PANEL_STAGE_START = 0,
	PANEL_STAGE_QSPI_GPIO,
	PANEL_STAGE_QSPI_PIO,
	PANEL_STAGE_QSPI_MODE,
	PANEL_STAGE_DMA,
	PANEL_STAGE_AMOLED_INIT,
	PANEL_STAGE_AMOLED_CLEAR,
	PANEL_STAGE_BRIGHTNESS,
	PANEL_STAGE_I2C,
	PANEL_STAGE_TOUCH_RESET,
	PANEL_STAGE_TOUCH_PROBE,
	PANEL_STAGE_TOUCH_RESTORE,
	PANEL_STAGE_FLUSH_IRQ,
	PANEL_STAGE_TOUCH_IRQ,
	PANEL_STAGE_DONE,

	/* Inside the render loop. Three of them, so a stall says which part of a
	   frame core 1 is stuck in. */
	PANEL_STAGE_UI_UPDATE,
	PANEL_STAGE_DRAW,
	PANEL_STAGE_PUSH
} PanelStage_t;

extern uint8_t Panel_Stage(void);
extern const char *Panel_StageName(uint8_t Stage);

/* Called by core 1 on its way round the loop: records where it is and ticks a
   counter. Core 0 watches the counter, so a stall is visible wherever it
   happens rather than only during bring-up. */
extern void Panel_Alive(uint8_t Stage);
extern uint32_t Panel_AliveCount(void);

/* THE FRAME LOOP. Core 1 draws on the panel's rhythm rather than on a timer:
 *
 *   Panel_TouchService()     fetch a touch report if the interrupt flagged one
 *   ...update the back buffer...
 *   Panel_PushPaletted()     send it, starting on the next TE pulse
 *   Panel_WaitFrame()        when there was nothing to send, wait for the
 *                            pulse anyway so the loop keeps the scan's cadence
 */
extern void Panel_WaitFrame(void);

/* Wait for the panel's next TE pulse. Panel_PushPaletted() does this itself;
   this is for a caller that wants the cadence without sending anything. */
extern void Panel_WaitTe(void);

/* Expand a rectangle to something the CO5300 can address: even columns and
   rows, clamped to the screen. The panel takes its column window in 2-pixel
   units and rounds an odd one itself, drawing the strip a pixel out - see the
   comment in panel.c. Inclusive coordinates, adjusted in place. */
extern void Panel_RoundArea(int32_t *X1, int32_t *Y1, int32_t *X2, int32_t *Y2);

/* SEND A PALETTED RECTANGLE, STARTING ON THE NEXT TE PULSE.
 *
 * Src is 8-bit palette indices, SrcStride bytes between rows; Palette is 256
 * RGB565 entries already in the panel's byte order. The rectangle is converted
 * and sent a chunk of lines at a time, the conversion of each chunk overlapping
 * the transmission of the one before it, under a single window and a single
 * chip select. Coordinates are inclusive and are rounded as above.
 *
 * Blocks until the last pixel has been clocked out. Core 1 only. */
extern void Panel_PushPaletted(const uint8_t *Src, uint32_t SrcStride,
                               const uint16_t *Palette,
                               int32_t X1, int32_t Y1, int32_t X2, int32_t Y2);

/* What the last push cost, and how the work divided.
 *
 * LastConvertUs and LastBlockedUs are the two halves of a frame's time on
 * core 1: converting, and waiting for the wire. Starved is the count of chunks
 * whose DMA had ALREADY finished when the CPU came back with the next one -
 * the bus idling for want of pixels. A frame rate held down by LastBlockedUs
 * is bus-bound and nothing but a smaller rectangle will help; one held down by
 * LastConvertUs with Starved climbing is CPU-bound, and the conversion loop is
 * where to look. LastTotalUs is measured from the TE edge itself, so it is
 * also how far into the scan the frame ran - the number to compare against the
 * 16.8 ms the panel gives. */
typedef struct
{
	uint32_t Frames;
	uint32_t Chunks;
	uint32_t Starved;
	uint32_t LastConvertUs;
	uint32_t LastBlockedUs;
	uint32_t LastTotalUs;
} PanelPush_t;

extern void Panel_Push(PanelPush_t *Out);

/* Panel brightness, 0 to 100 percent. A dashboard gauge at full brightness at
   night is a hazard, so this exists to be driven from something - a light
   sensor, the car's illumination line, or a page - later. */
extern void Panel_SetBrightness(uint8_t Percent);

/* Diagnostics for the console status line. The board's USB console discards
   everything printed before a host attaches, so anything worth knowing has to
   be reachable from a periodic line rather than a boot banner.

   Panel_FlushTimeouts() is the one that should stay at zero: see the comment
   on the drain in panel.c.

   The two touch counters answer different questions, which is why there are
   two. Panel_TouchReports() counts replies that carry the controller's touch
   report marker, so it climbs steadily whenever the controller is in
   reporting mode at all - stuck at zero means it is not. Panel_TouchPresses()
   counts the subset with a finger in them. */
extern uint32_t Panel_Flushes(void);

/* TE sync. Edges should climb at the panel frame rate and PeriodUs sit near
   16,667; Enabled goes false only if no edge ever arrived. AvgWaitUs is the
   latency the sync adds to a frame. */
typedef struct
{
	uint32_t Edges;
	uint32_t PeriodUs;
	bool Enabled;
	uint32_t Waits;
	uint32_t Timeouts;
	uint32_t AvgWaitUs;
	uint32_t LateFrames;		/* frames that missed a scan while busy */
} PanelTe_t;

extern void Panel_Te(PanelTe_t *Out);

/* Cumulative since boot, for benchmarking a build: diff two snapshots taken a
   few seconds apart for pixels per second and mean frame time. */
extern uint64_t Panel_RenderTotalUs(void);
extern uint64_t Panel_RenderTotalPx(void);

/* What the panel costs on the bus, measured rather than derived.
   Panel_FlushBusyUsPerFrame() is the one M4 wants: what competes with can2040
   is the share of wall-clock time the panel is mid-burst, not the frame rate.
   Panel_DrainSpinsMax() is the evidence that the bound on the PIO drain in
   Panel_FlushDoneIrq() is generous rather than lucky. */
extern uint32_t Panel_FlushMbPerSx10(void);
extern uint32_t Panel_FlushBusyUsPerFrame(void);
extern uint32_t Panel_DrainSpinsMax(void);

/* Areas that had to be aligned to even columns - see Panel_RoundArea(). */
extern uint32_t Panel_RoundedAreas(void);

/* Both should stay at zero. Panel_FlushOverlaps() counts flushes that arrived
   with the previous transfer still running; Panel_FlushCsOverlaps() counts
   those that arrived with chip select still low, meaning the completion
   handler had not run. Either would corrupt where a section lands. */
extern uint32_t Panel_FlushOverlaps(void);
extern uint32_t Panel_FlushCsOverlaps(void);
extern uint32_t Panel_FlushTimeouts(void);
extern bool Panel_TouchPresent(void);
extern uint16_t Panel_TouchChipType(void);
extern uint32_t Panel_TouchReports(void);

/* Interrupt edges seen on the touch line, counted per direction. The line
   idles low and pulses high for a report, so the rising count is the one that
   should move under a finger - and if neither moves, the pin in DEV_Config.h
   is not the one the controller is wired to. Worth keeping: the board's
   published pinout is already known to be wrong twice (vendor/README.md). */
extern uint32_t Panel_TouchRiseEdges(void);
extern uint32_t Panel_TouchFallEdges(void);
extern uint32_t Panel_TouchPresses(void);
extern void Panel_TouchLast(uint16_t *X, uint16_t *Y);

/* Fetch a touch report if the interrupt flagged one, and age out a press whose
   reports have stopped. Once per frame, core 1. */
extern void Panel_TouchService(void);

/* Is a finger down, and where was it last seen? The position is held across a
   release so a gesture can be measured between press and release. */
extern bool Panel_TouchDown(int32_t *X, int32_t *Y);

#endif /* PANEL_H_ */
