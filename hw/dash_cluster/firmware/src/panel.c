/*
 * panel.c
 *
 * The CO5300 AMOLED and the CST9217 touch controller: the transport, and
 * nothing above it.
 *
 * The transport, the panel init sequence and the touch register map all come
 * from Waveshare's drivers in firmware/vendor/ - see vendor/README.md for
 * provenance. Those files are kept as delivered so the diff against a future
 * vendor release stays readable, which means the places where their example
 * is wrong are corrected here instead of there. Three of them:
 *
 *  1. THE DRAW BUFFER. Their example allocates
 *     malloc(DISP_HOR_RES * DISP_VER_RES) - a count in BYTES - and then uses
 *     it as though it held that many PIXELS: a 212 KB overflow. There is no
 *     such buffer here at all. The renderer keeps one 8-bit paletted back
 *     buffer (ui_draw.c) and this file converts it to RGB565 a chunk of lines
 *     at a time on its way out, so nothing ever holds a 434 KB frame - which
 *     is just as well, since no PSRAM is fitted (measured, test/psram_probe.c).
 *
 *  2. CHIP SELECT AFTER THE DMA. Their completion handler raises chip select
 *     as soon as the DMA finishes, but a finished DMA only means the last byte
 *     reached the PIO FIFO - not that it has been clocked out. See the drain
 *     in Panel_FlushDoneIrq().
 *
 *  3. TOUCH AS AN INTERRUPT. Their handler latches a press per interrupt and
 *     releases it on the next read, so a swipe can only ever read as a tap.
 *     Swiping between faces needs the press held while the finger moves. See
 *     Panel_TouchService().
 *
 * Everything here runs on core 1 except Panel_ClockInit(). Nothing in this
 * file may be called from core 0.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/pio.h"

/* Vendor. DEV_Config.h carries the pin map and declares the globals dma_tx
   and c - the DMA channel and its config - that their panel driver transmits
   through. The name `c` for a global is theirs; do not shadow it here. */
#include "DEV_Config.h"
#include "AMOLED_1in75.h"
#include "CST9217.h"
#include "qspi_pio.h"

#include "panel.h"

/* THE CHUNK, AND WHY THE FRAME IS NOT HELD IN 16-BIT FORM ANYWHERE.
 *
 * A 466x466 RGB565 frame is 434 KB and does not fit. The back buffer is 8-bit
 * paletted (ui_draw.c, 212 KB) and this file converts it on the way out, a
 * few lines at a time, into a scratch buffer small enough to be free:
 * 466 x 8 pixels is 7,456 bytes, and there are two so the CPU can build the
 * next chunk while the DMA is still clocking out the last one.
 *
 * Eight lines is chosen so the transfer is comfortably longer than the
 * conversion. At the measured 46.9 MB/s a chunk takes 159 us on the wire; the
 * conversion is a byte load, a table lookup and a halfword store per pixel.
 * If the CPU ever falls behind, nothing breaks - the PIO simply stalls with
 * chip select still low and the frame takes longer. PanelPush_t.Starved
 * counts the opposite case, where the DMA finished first and the bus went
 * idle, which is what says the split is generous. */
#define PANEL_CHUNK_LINES	(8u)
#define PANEL_CHUNK_PIXELS	((uint32_t)PANEL_WIDTH * PANEL_CHUNK_LINES)

/* Bound on the PIO drain below. The FIFO holds four words plus one in the
   output shift register - at most 40 nibbles, around 400 ns - so this is
   roughly a hundred times the time it should ever need. Bounded rather than
   open because this spins inside an interrupt, and a bus drain that can hang
   forever is a fault this repo has been bitten by before. */
#define PANEL_DRAIN_SPINS	(1000u)

/* Percent, from the build - see DASH_BRIGHTNESS in CMakeLists.txt, which keeps
   bench builds dim so a static dial does not burn into the AMOLED. */
#ifndef DASH_BRIGHTNESS
#define DASH_BRIGHTNESS			(20)
#endif
#define PANEL_DEFAULT_BRIGHTNESS	((uint8_t)DASH_BRIGHTNESS)

/* The panel's tearing-effect output. Not in Waveshare's pin map or sources at
   all - GPIO17 comes from the board schematic. */
#define PANEL_TE_PIN		(17u)

/* How long a flush waits for a TE pulse before giving up on it. A 60 Hz frame
   is 16.7 ms, so this covers one missed pulse with room to spare. */
#define PANEL_TE_TIMEOUT_US	(40000u)

/* Timeouts with no TE edge ever seen before sync is abandoned. A panel that
   never pulses TE would otherwise cost every single frame the full timeout. */
#define PANEL_TE_GIVE_UP	(5u)

/* The two conversion scratch buffers, and the state the TE interrupt needs to
   start the first chunk of a frame the moment the pulse arrives. */
static uint16_t			ChunkBuf[2][PANEL_CHUNK_PIXELS];

/* Set by Panel_PushPaletted() once the window is open and the first chunk is
   converted; cleared by the TE interrupt as it starts that chunk's DMA.
   volatile because the two are different contexts. */
static volatile bool		PushArmed;
static const void	       *PushArmData;
static uint32_t			PushArmBytes;
static volatile uint32_t	PushStartUs;

static uint32_t			PushFrames;
static uint32_t			PushChunks;
static uint32_t			PushStarved;
static uint32_t			PushLastConvertUs;
static uint32_t			PushLastBlockedUs;
static uint32_t			PushLastTotalUs;

/* Within a frame: conversion and waiting summed over its rectangles, and how
   many rectangles it had. Rows whose spans came up short of the rectangle's
   width - a composing bug, filled with black and counted. */
static uint32_t			SendConvertUs;
static uint32_t			SendBlockedUs;
static uint32_t			PushRegionsLast;
static uint32_t			RowShortfalls;

/* Last known touch position, held across releases - see Panel_TouchRead(). */
static int32_t			TouchX;
static int32_t			TouchY;

static uint32_t			Flushes;
static uint32_t			FlushTimeouts;

/* Cumulative render time and pixels over every frame pushed, for
   benchmarking: diff two snapshots to get throughput over a window.
   Microseconds, because a millisecond tick quantises a 9 ms frame by 11%. */
static uint64_t			RenderTotalUs;
static uint64_t			RenderTotalPx;

/* How long the panel holds the bus, and how much it moves while holding it.
   64-bit because a full screen is 434 KB and this would wrap a 32-bit byte
   count in about ten minutes of steady rendering. */
static uint64_t			FlushBytes;
static uint64_t			FlushBusyUs;
static uint32_t			FlushStartUs;
static uint32_t			DrainSpinsMax;

/* Invalidated areas that needed aligning to even columns. If this is moving,
   the panel was being handed odd column windows - see Panel_Rounder(). */
static uint32_t			RoundedAreas;

/* TE sync. The edge count and the time between the last two edges are written
   by the GPIO interrupt; the rest by the flush. */
static volatile uint32_t	TeEdges;
static volatile uint32_t	TeLastUs;
static volatile uint32_t	TePeriodUs;
static bool			TeSyncEnabled = true;
static uint32_t			TeSyncWaits;
static uint32_t			TeSyncTimeouts;
static uint64_t			TeWaitUs;

/* Late frames: a push that went out two or more scans after the previous one
   although the renderer had been busy the whole time - a scan genuinely
   missed. A frame that deliberately sent nothing - the needle had not moved,
   or a strip chart's next column was not due - waits for the pulse through
   Panel_WaitFrame() and excuses the gap, or a page that redraws at 30 Hz would
   report every second frame as late. */
static uint32_t			TeLateFrames;
static uint32_t			TeLastPushEdge;
static bool			FrameIdled = true;

/* Written by core 1 as Panel_Init() advances, read by core 0 so a hang can be
   named rather than guessed at. See the note in panel.h. */
static volatile uint8_t		InitStage = PANEL_STAGE_START;
static volatile uint32_t	AliveCount;

/* Flushes that arrived while the previous transfer was still in flight. Should
   be impossible - LVGL's draw_buf_flush() waits on its `flushing` flag before
   calling flush_cb, and that flag is cleared by our completion interrupt - but
   "should" is not evidence, and starting a window write over a running
   transfer would corrupt the tail of one section and the placement of the
   next. Measured rather than assumed. */
static uint32_t			FlushOverlaps;
static uint32_t			FlushCsOverlaps;
static uint32_t			TouchReports;
static uint32_t			TouchPresses;

/* Set by the interrupt, cleared by the read callback. volatile because those
   are different contexts; a single flag needs no more than that. */
static volatile bool		TouchIntPending;
static volatile uint32_t	TouchRiseEdges;
static volatile uint32_t	TouchFallEdges;

/* The held press, and when it was last confirmed by a report. */
static bool			TouchHeld;
static uint32_t			TouchLastReportMs;

/* How long a press survives with no further report before it is treated as a
   lift. The controller reports continuously while a finger is down, at well
   under this interval, so a gap this long means the finger has gone and the
   final report was missed.

   A FALLBACK ONLY. Lifts are found from the controller's own lift report -
   see Panel_TouchReadReport() - which on the bench caught 17 of 17. Before
   that fix every lift waited out this timeout, and the swipe stalled for it on
   each release; that is why it was once cut to 40 ms. It is long again
   because TouchGapMaxMs has seen 51 ms between reports under a finger, and a
   timeout inside a real gap would break a drag in two. */
#define TOUCH_HOLD_TIMEOUT_MS	(100u)

/* How lifts were noticed - a report saying the finger had gone, or the timeout
   above - and the longest gap between reports while a finger was down. */
static uint32_t			TouchLiftReports;
static uint32_t			TouchLiftTimeouts;
static uint32_t			TouchGapMaxMs;

/* Every status nibble seen in a report, as a bit mask, so which value means
   "lifted" is visible on the console rather than assumed; and bounded reads
   that failed. */
static uint16_t			TouchStatusSeen;
static uint32_t			TouchReadErrors;
static bool			TouchPresent;
static uint16_t			TouchChipType;


/***************************************************************************************/
void Panel_ClockInit(void)
{
	/* 200 MHz, which is what the vendor's PIO clock divider of 1.0 is chosen
	   against - the QSPI bit rate follows the system clock directly, so
	   changing this changes the panel's clock too.

	   Core 0, before stdio and before CanLink_Init(): can2040 computes its bit
	   timing from clock_get_hz(clk_sys) once at startup, so a clock change
	   afterwards would silently put every CAN bit at the wrong length. */
	set_sys_clock_khz(PLL_SYS_KHZ, true);

	/* The peripheral clock does not follow clk_sys automatically. The I2C the
	   touch controller sits on is clocked from here. */
	clock_configure(clk_peri,
	                0,
	                CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
	                PLL_SYS_KHZ * 1000,
	                PLL_SYS_KHZ * 1000);
}


/***************************************************************************************/
/* Expand an area to what the panel can actually address.
 *
 * The CO5300 takes its column window in 2-pixel units, so an odd column start
 * is rounded by the panel and the strip is drawn one pixel out. That went
 * unnoticed until page transitions existed, and then only in one direction:
 * sliding a face in from the right puts the invalidated rectangle at the
 * animated x position, which is odd half the time, while sliding one in from
 * the left anchors it at x1 = 0 every frame.
 *
 * ROWS NEED THE SAME TREATMENT, AND FOR A LESS OBVIOUS REASON.
 *
 * A horizontal slide invalidates the full height, so the animation's own
 * rectangle always has y1 = 0. But LVGL splits an area too big for the draw
 * buffer into horizontal bands, and the band height it picks is
 * buffer_pixels / area_width - so it depends on the width, which changes every
 * frame of a slide. A narrow rectangle gives tall bands: at 100 px wide the
 * bands are 279 rows, and the second one starts at y1 = 279. Odd. The panel
 * rounds it, that band is drawn one row out, and the result is graphics that
 * appear to jitter up and down by a pixel at random - random because it
 * depends on a width that is different every frame.
 *
 * Rounding rows here fixes it properly rather than by luck: LVGL's
 * get_max_row() probes this callback and shrinks the band height until the
 * ROUNDED height still fits the buffer, then uses that as its step. Round the
 * rows and it picks an even band height, so every band starts on an even row.
 *
 * Rounding here rather than in the flush is the whole point: LVGL renders the
 * expanded rectangle, so the window and the buffer contents still describe the
 * same pixels. Widening it in the flush instead would send the wrong pixels
 * for the added column or row.
 *
 * The banding described above was LVGL's; this renderer sends whole
 * rectangles. The column rule is the panel's own, though, and outlives it -
 * every window handed to AMOLED_1IN75_SetWindows() goes through here.
 *
 * Start down to even and end up to odd also makes every width and height even.
 * Both clamps land on values that already have the right parity - 0 is even,
 * 465 is odd - so clamping cannot put the alignment back. */
void Panel_RoundArea(int32_t *X1, int32_t *Y1, int32_t *X2, int32_t *Y2)
{
	bool Needed;

	Needed = ((*X1 & 1) != 0) || ((*X2 & 1) == 0)
	         || ((*Y1 & 1) != 0) || ((*Y2 & 1) == 0);

	if ((*X1 & 1) != 0)
		*X1 = *X1 - 1;
	if ((*X2 & 1) == 0)
		*X2 = *X2 + 1;

	if ((*Y1 & 1) != 0)
		*Y1 = *Y1 - 1;
	if ((*Y2 & 1) == 0)
		*Y2 = *Y2 + 1;

	if (*X1 < 0)
		*X1 = 0;
	if (*X2 > (int32_t)(PANEL_WIDTH - 1))
		*X2 = (int32_t)(PANEL_WIDTH - 1);
	if (*Y1 < 0)
		*Y1 = 0;
	if (*Y2 > (int32_t)(PANEL_HEIGHT - 1))
		*Y2 = (int32_t)(PANEL_HEIGHT - 1);

	if (Needed)
		RoundedAreas++;
}


/***************************************************************************************/
/* Hold a flush until the panel's next TE pulse.
 *
 * WHY THIS CLOSES THE TEAR WINDOW RATHER THAN NARROWING IT.
 *
 * The panel scans out of its own memory, top to bottom, at its own rate, and a
 * tear is the scan passing through a region while it is being written. TE
 * rises as the scan reaches line 471 - set by the vendor init with 44h, just
 * past the 466 visible rows - so it marks vertical blanking. A write that
 * starts then has the whole visible frame ahead of the scan, and because the
 * one-burst flush writes rows faster than the scan draws them (roughly 25 us a
 * row against 35 us a line), the scan can never catch it up. So it holds for
 * any region, top of the dial or bottom.
 *
 * Always the NEXT edge, not merely a recent one: the render before this flush
 * takes about as long as a frame, so an edge that happened during it is
 * already history by the time the write would start.
 *
 * The cost is latency, up to one frame and about half a frame on average, with
 * the frame rate capped at the panel's own 60 Hz - which is roughly where this
 * renderer was anyway.
 */
void Panel_WaitTe(void)
{
	uint32_t Seen, Start;

	if (!TeSyncEnabled)
		return;

	Seen = TeEdges;
	Start = time_us_32();

	while (TeEdges == Seen)
	{
		if ((uint32_t)(time_us_32() - Start) > PANEL_TE_TIMEOUT_US)
		{
			TeSyncTimeouts++;

			/* Never seen a single edge: TE is not arriving at all, so stop
			   paying for it on every frame and fall back to unsynced. */
			if (TeEdges == 0u && TeSyncTimeouts >= PANEL_TE_GIVE_UP)
				TeSyncEnabled = false;
			return;
		}
		tight_loop_contents();
	}

	TeSyncWaits++;
	TeWaitUs += (uint32_t)(time_us_32() - Start);
}


/***************************************************************************************/
/* Is the touch controller on the bus, and is it the part expected?
 *
 * Their CST9217_Read_Config() cannot answer this, for two reasons. It throws
 * away every I2C return code, so a completely dead bus reads back as zeroes
 * and becomes a plausible-looking wrong answer. And the command-mode write it
 * opens with is broken: CST9217_I2C_Write_nByte() builds a buffer of `2 + Len`
 * bytes - two of register address followed by the payload - then passes `Len`
 * as the length, so the payload is dropped and only the address bytes go out.
 * It goes unnoticed there because the register it happens to write (0xD101)
 * has the same two bytes as the payload it meant to send.
 *
 * This is also the only moment the part can be asked anything: it answers
 * immediately after a reset and then goes unresponsive until a touch wakes it.
 *
 * EVERY TRANSFER HERE IS BOUNDED, AND THAT IS NOT DECORATION.
 *
 * i2c_write_blocking() with nostop set returns an error on a NAK but leaves
 * the bus without a STOP, and the next transfer on it can then block forever.
 * That is not hypothetical: an earlier version of this probe did exactly that,
 * and the hang landed in the LVGL input callback - before the first status
 * line could be printed, so the board enumerated over USB and said nothing at
 * all. A touch controller that does not answer must cost this function a few
 * milliseconds, not the display.
 */
#define TOUCH_I2C_TIMEOUT_US	(5000u)

static bool Panel_TouchProbe(uint16_t *ChipType)
{
	/* Address, then payload - the whole four bytes, which is the part their
	   helper drops. 0xD101 with {0xD1, 0x01} is their command-mode entry. */
	static const uint8_t EnterCommandMode[4] = { 0xD1u, 0x01u, 0xD1u, 0x01u };
	uint8_t Addr[2];
	uint8_t Data[4] = { 0, 0, 0, 0 };

	if (i2c_write_timeout_us(I2C_PORT, CST9217_I2C_ADDR, EnterCommandMode,
	                         sizeof(EnterCommandMode), false,
	                         TOUCH_I2C_TIMEOUT_US)
	    != (int)sizeof(EnterCommandMode))
		return false;

	sleep_ms(10);

	Addr[0] = (uint8_t)(CST9217_PROJECT_ID_REG >> 8);
	Addr[1] = (uint8_t)(CST9217_PROJECT_ID_REG & 0xFFu);

	/* nostop: the register address and the read are one transaction, so the
	   read below issues a repeated start. If it fails, the recovery read
	   afterwards is what puts a STOP back on the bus. */
	if (i2c_write_timeout_us(I2C_PORT, CST9217_I2C_ADDR, Addr, sizeof(Addr),
	                         true, TOUCH_I2C_TIMEOUT_US) != (int)sizeof(Addr))
	{
		(void)i2c_read_timeout_us(I2C_PORT, CST9217_I2C_ADDR, Data, 1, false,
		                          TOUCH_I2C_TIMEOUT_US);
		return false;
	}

	if (i2c_read_timeout_us(I2C_PORT, CST9217_I2C_ADDR, Data, sizeof(Data),
	                        false, TOUCH_I2C_TIMEOUT_US) != (int)sizeof(Data))
		return false;

	*ChipType = (uint16_t)(((uint16_t)Data[3] << 8) | (uint16_t)Data[2]);
	return true;
}


/***************************************************************************************/
/* Runs on core 1, where it is enabled. Deliberately does nothing but flag and
   count: the report is fetched over I2C by the read callback, because a
   blocking transfer inside an interrupt is how a sulking touch controller
   would take the renderer down with it. */
static void Panel_GpioIrq(uint Gpio, uint32_t Events)
{
	/* ONE CALLBACK FOR EVERY GPIO ON THIS CORE, BY NECESSITY.
	   The SDK keeps a single GPIO interrupt callback per core, and
	   gpio_set_irq_enabled_with_callback() replaces it. Registering TE with a
	   callback of its own would silently disconnect touch, so both are
	   dispatched from here. */
	if (Gpio == PANEL_TE_PIN)
	{
		uint32_t Now = time_us_32();

		if (TeEdges != 0u)
			TePeriodUs = Now - TeLastUs;
		TeLastUs = Now;
		TeEdges++;

		/* THE FRAME STARTS HERE, NOT WHEN CORE 1 NOTICES THE EDGE.
		 *
		 * Panel_PushPaletted() opens the window, converts the first chunk and
		 * arms; the transfer itself begins in this handler, so the pixels
		 * start moving within the interrupt latency of the pulse rather than
		 * after core 1 has come round its wait loop. Everything needed is
		 * already in memory, so this is one DMA register write group and
		 * nothing else. */
		if (PushArmed)
		{
			PushStartUs = Now;
			FlushStartUs = Now;
			PushArmed = false;
			dma_channel_configure(dma_tx, &c, &qspi.pio->txf[qspi.sm],
			                      PushArmData, PushArmBytes, true);
		}
		return;
	}

	if (Gpio != TOUCH_INT_PIN)
		return;

	if ((Events & GPIO_IRQ_EDGE_RISE) != 0u)
		TouchRiseEdges++;
	if ((Events & GPIO_IRQ_EDGE_FALL) != 0u)
		TouchFallEdges++;

	TouchIntPending = true;
}


/***************************************************************************************/
/* Read the touch controller.
 *
 * INTERRUPT TRIGGERED, WITH THE PRESS HELD BETWEEN REPORTS.
 *
 * Neither the vendor's arrangement nor straight polling works here.
 *
 * Polling does not work because the part does not answer when it is idle.
 * Measured on the board with nothing touching the glass: most reads of the
 * report register return all-FF, the rest return an unchanging stale frame,
 * and the byte the decode wants to be 0xAB is 0x07. It answers right after a
 * reset and then stops. So the interrupt is not a convenience, it is the only
 * time there is anything to read.
 *
 * The vendor's arrangement does not work either: their handler latches a press
 * and their read callback clears it on the next call, so LVGL gets a press and
 * a release around one coordinate - a tap - whatever the finger actually did.
 * A swipe needs the press asserted while the finger travels, or LVGL never
 * accumulates the distance it needs to call it a gesture, and swiping between
 * faces is the reason there is a touch panel here at all (PLAN.md 4.3).
 *
 * So the interrupt says when a report exists, and the press is held until
 * either a report says the finger has gone or nothing arrives for
 * TOUCH_HOLD_TIMEOUT_MS. Holding it forever would be worse than not holding it
 * at all - a stuck press is a finger the UI believes is still down.
 */
/* The report, read here rather than through CST9217_Read_Data().
 *
 * THEIR DECODE CANNOT SEE A LIFT. Each point in the report carries a status
 * nibble, 0x06 while the finger is down. The report the controller sends as
 * the finger comes up still says one point, with a different status - and
 * their decode, which only copies coordinates when the status is 0x06,
 * returns success with points = 1 and the old coordinates left in place. So
 * every lift arrived here as one more press, and only the hold timeout ever
 * ended a touch: measured, 19 lifts out of 19. The timeout is 40 ms, and the
 * swipe stalled for that long on every release.
 *
 * Their read is also unbounded - i2c_write_blocking() with nostop - which is
 * the hang this file is careful about everywhere else.
 *
 * Returns false if there was no report to read. Status is the first point's
 * status nibble, 0 when there are no points. */
static bool Panel_TouchReadReport(bool *Pressed, uint16_t *X, uint16_t *Y,
                                  uint8_t *Status)
{
	uint8_t Addr[2];
	uint8_t Data[CST9217_DATA_LENGTH];
	uint8_t Points;

	Addr[0] = (uint8_t)(CST9217_DATA_REG >> 8);
	Addr[1] = (uint8_t)(CST9217_DATA_REG & 0xFFu);

	if (i2c_write_timeout_us(I2C_PORT, CST9217_I2C_ADDR, Addr, sizeof(Addr),
	                         true, TOUCH_I2C_TIMEOUT_US) != (int)sizeof(Addr))
	{
		/* The recovery read is what puts a STOP back on the bus. */
		(void)i2c_read_timeout_us(I2C_PORT, CST9217_I2C_ADDR, Data, 1, false,
		                          TOUCH_I2C_TIMEOUT_US);
		TouchReadErrors++;
		return false;
	}

	if (i2c_read_timeout_us(I2C_PORT, CST9217_I2C_ADDR, Data, sizeof(Data), false,
	                        TOUCH_I2C_TIMEOUT_US) != (int)sizeof(Data))
	{
		TouchReadErrors++;
		return false;
	}

	/* Byte 6 marks a touch report; anything else is not one. */
	if (Data[6] != CST9217_ACK_VALUE)
		return false;

	Points = (uint8_t)(Data[5] & 0x7Fu);
	*Status = (Points > 0u) ? (uint8_t)(Data[0] & 0x0Fu) : 0u;
	*Pressed = (Points > 0u) && (*Status == 0x06u);

	/* Coordinates are 12 bits split over three bytes, and the panel is mounted
	   rotated 180 degrees against the touch sensor. A reading beyond the panel
	   is clamped to its edge rather than wrapped by the subtraction. */
	{
		int32_t Rx = (int32_t)((((uint32_t)Data[1]) << 4) | (((uint32_t)Data[3]) >> 4));
		int32_t Ry = (int32_t)((((uint32_t)Data[2]) << 4) | (((uint32_t)Data[3]) & 0x0Fu));
		int32_t Fx = 466 - Rx;
		int32_t Fy = 466 - Ry;

		*X = (uint16_t)((Fx < 0) ? 0 : Fx);
		*Y = (uint16_t)((Fy < 0) ? 0 : Fy);
	}
	return true;
}


/***************************************************************************************/
void Panel_TouchService(void)
{
	uint32_t NowMs = to_ms_since_boot(get_absolute_time());
	bool Pressed;
	uint16_t X, Y;
	uint8_t Status;

	if (TouchIntPending)
	{
		TouchIntPending = false;

		if (Panel_TouchReadReport(&Pressed, &X, &Y, &Status))
		{
			TouchReports++;
			TouchStatusSeen |= (uint16_t)(1u << Status);

			if (Pressed)
			{
				/* 466 - 0 is 466 and the last valid pixel is 465, so clamp
				   rather than report a point one off the edge - and a wild
				   reading, which the subtraction would wrap, the same way. */
				if (X >= PANEL_WIDTH)
					X = PANEL_WIDTH - 1u;
				if (Y >= PANEL_HEIGHT)
					Y = PANEL_HEIGHT - 1u;

				TouchX = (int32_t)X;
				TouchY = (int32_t)Y;
				TouchPresses++;
				if (TouchHeld && (NowMs - TouchLastReportMs) > TouchGapMaxMs)
					TouchGapMaxMs = NowMs - TouchLastReportMs;
				TouchHeld = true;
				TouchLastReportMs = NowMs;
			}
			else
			{
				if (TouchHeld)
					TouchLiftReports++;
				TouchHeld = false;
			}
		}
	}
	else if (TouchHeld && (NowMs - TouchLastReportMs) > TOUCH_HOLD_TIMEOUT_MS)
	{
		TouchLiftTimeouts++;
		TouchHeld = false;
	}
}


/***************************************************************************************/
void Panel_TouchLifts(uint32_t *ByReport, uint32_t *ByTimeout, uint32_t *GapMaxMs,
                      uint16_t *StatusSeen, uint32_t *ReadErrors)
{
	*ByReport = TouchLiftReports;
	*ByTimeout = TouchLiftTimeouts;
	*GapMaxMs = TouchGapMaxMs;
	*StatusSeen = TouchStatusSeen;
	*ReadErrors = TouchReadErrors;
}


/***************************************************************************************/
/* The press, and where it is. The position is the last one seen, held across a
   release: a gesture is measured between the press and the release, and (0,0)
   on the releasing read would look like a sudden drag to the corner. */
bool Panel_TouchDown(int32_t *X, int32_t *Y)
{
	*X = TouchX;
	*Y = TouchY;
	return TouchHeld;
}


/***************************************************************************************/
bool Panel_Init(void)
{
	bool Acked;

	/* ---- transport ---------------------------------------------------- */

	/* Their DEV_Module_Init() is deliberately not called. It would do four
	   useful things and two harmful ones: it calls stdio_init_all() a second
	   time, and it changes the system clock - which has to happen on core 0
	   before can2040 starts, not here. So the useful parts are spelled out. */
	QSPI_GPIO_Init(qspi);
	InitStage = PANEL_STAGE_QSPI_GPIO;
	QSPI_PIO_Init(qspi);
	InitStage = PANEL_STAGE_QSPI_PIO;

	/* THEIR PIO INIT LEAVES THE STATE MACHINE DISABLED, AND NOTHING RE-ENABLES
	   IT.

	   qspi_4wire_data_program_init() enables the state machine, and then
	   QSPI_PIO_Init() calls pio_sm_set_enabled(..., false) on it immediately
	   afterwards - undoing its own setup. Every QSPI_4Wrie_Mode() call inside
	   their panel driver is commented out, so nothing in the sources they ship
	   ever turns it back on: their own example must do this in a main() that
	   was not part of the driver directory.

	   Without it the first register write hangs. QSPI_PIO_Write() is
	   pio_sm_put_blocking(), so it fills the four-word FIFO and then waits
	   forever for a state machine that is not running - which presents as a
	   board that enumerates over USB, prints nothing, and never reaches its
	   first line of output. */
	QSPI_4Wrie_Mode(&qspi);
	InitStage = PANEL_STAGE_QSPI_MODE;

	dma_tx = (uint)dma_claim_unused_channel(true);
	c = dma_channel_get_default_config(dma_tx);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));
	InitStage = PANEL_STAGE_DMA;

	/* ---- panel -------------------------------------------------------- */

	AMOLED_1IN75_Init();
	InitStage = PANEL_STAGE_AMOLED_INIT;
	AMOLED_1IN75_Clear(BLACK);
	InitStage = PANEL_STAGE_AMOLED_CLEAR;
	AMOLED_1IN75_SetBrightness(PANEL_DEFAULT_BRIGHTNESS);
	InitStage = PANEL_STAGE_BRIGHTNESS;

	/* ---- touch -------------------------------------------------------- */

	i2c_init(I2C_PORT, 400 * 1000);
	gpio_set_function(DEV_SDA_PIN, GPIO_FUNC_I2C);
	gpio_set_function(DEV_SCL_PIN, GPIO_FUNC_I2C);
	gpio_pull_up(DEV_SDA_PIN);
	gpio_pull_up(DEV_SCL_PIN);
	InitStage = PANEL_STAGE_I2C;

	gpio_init(TOUCH_RST_PIN);
	gpio_set_dir(TOUCH_RST_PIN, GPIO_OUT);

	/* The interrupt line. No internal pull is enabled: a pull-down on a
	   digital input can latch at about 2.2 V on RP2350 (erratum E9), and the
	   board drives this line itself.

	   Measured idle low with nothing touching the glass, so a report is
	   signalled by the rising edge - which is what the vendor example's
	   GPIO_IRQ_EDGE_RISE implies too. Both edges are taken anyway: the falling
	   one is a free chance to notice the finger has gone, and the two counters
	   make the polarity self-evident rather than something to trust. */
	gpio_init(TOUCH_INT_PIN);
	gpio_set_dir(TOUCH_INT_PIN, GPIO_IN);

	/* CST9217_Init() would read the configuration and throw the result away.
	   The interrupt pin is left untouched - see Panel_TouchRead(). */
	CST9217_Reset();
	sleep_ms(30);
	InitStage = PANEL_STAGE_TOUCH_RESET;
	Acked = Panel_TouchProbe(&TouchChipType);
	TouchPresent = Acked && (TouchChipType == CST9217_CHIP_ID);
	InitStage = PANEL_STAGE_TOUCH_PROBE;

	/* THE PROBE LEAVES THE CONTROLLER IN COMMAND MODE, AND IT HAS TO COME OUT.
	 *
	 * Reading the chip type needs command mode, and in command mode the
	 * controller answers with configuration rather than touch reports - so
	 * leaving it there means touch never works at all. That is not
	 * theoretical: it is what made the first working display have a dead
	 * touch panel.
	 *
	 * The vendor driver has no exit for this because it never entered the
	 * mode in the first place - CST9217_I2C_Write_nByte() drops its payload
	 * (finding 5 in vendor/README.md), so their command-mode write was inert.
	 * Fixing that length made the mode change real and this exit necessary.
	 *
	 * A second reset rather than a guessed exit-command register: the
	 * power-on state is reporting mode, and a reset is a mechanism already
	 * proven on this board rather than one inferred from a family datasheet.
	 * It costs about 110 ms, once. */
	CST9217_Reset();
	sleep_ms(30);
	InitStage = PANEL_STAGE_TOUCH_RESTORE;

	/* Start centred, so a release before any press cannot read as a gesture
	   from the corner. */
	TouchX = PANEL_WIDTH / 2;
	TouchY = PANEL_HEIGHT / 2;

	/* ---- the transfer channel ----------------------------------------- */

	/* Clear any latched completion. AMOLED_1IN75_Clear() above runs 466 polled
	   DMA transfers, which leave the channel's interrupt status set; the push
	   polls the channel rather than taking an interrupt, but leaving a stale
	   flag behind is the sort of thing that confuses the next person to
	   attach a debugger. */
	dma_channel_acknowledge_irq0(dma_tx);
	InitStage = PANEL_STAGE_FLUSH_IRQ;

	/* Likewise core 1, and for the same reason: the flag it sets is read by
	   the LVGL callback. Only enabled once the controller is out of reset and
	   has been identified, so a reset pulse cannot be counted as a report.
	   Core 1, because that is where the renderer reads it. */
	if (TouchPresent)
		gpio_set_irq_enabled(TOUCH_INT_PIN,
		                     GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

	/* TE. An input the panel drives, so no pull: a pull-down on a digital
	   input can latch at about 2.2 V on RP2350 (erratum E9), and nothing is
	   gained by one here. Registering it is also what installs the shared
	   callback, which is why touch above only enables its events - see
	   Panel_GpioIrq(). Order does not matter: both are enabled before
	   anything is drawn. */
	gpio_init(PANEL_TE_PIN);
	gpio_set_dir(PANEL_TE_PIN, GPIO_IN);
	gpio_disable_pulls(PANEL_TE_PIN);
	gpio_set_irq_enabled_with_callback(PANEL_TE_PIN, GPIO_IRQ_EDGE_RISE,
	                                   true, Panel_GpioIrq);

	InitStage = PANEL_STAGE_TOUCH_IRQ;

	printf("panel: %dx%d, %u-line chunks (2 x %lu bytes)\n",
	       PANEL_WIDTH, PANEL_HEIGHT, (unsigned)PANEL_CHUNK_LINES,
	       (unsigned long)sizeof(ChunkBuf[0]));
	if (TouchPresent)
		printf("touch: CST9217 on the bus\n");
	else if (Acked)
		printf("touch: something answered at 0x%02X but reports chip type "
		       "0x%04X, expected 0x%04X\n",
		       CST9217_I2C_ADDR, TouchChipType, CST9217_CHIP_ID);
	else
		printf("touch: no answer at 0x%02X - swiping unavailable\n",
		       CST9217_I2C_ADDR);

	InitStage = PANEL_STAGE_DONE;
	return TouchPresent;
}


/***************************************************************************************/
uint8_t Panel_Stage(void)
{
	return InitStage;
}


void Panel_Alive(uint8_t Stage)
{
	InitStage = Stage;
	AliveCount++;
}


uint32_t Panel_AliveCount(void)
{
	return AliveCount;
}


const char *Panel_StageName(uint8_t Stage)
{
	static const char *const Names[] =
	{
		"start", "qspi-gpio", "qspi-pio", "qspi-mode", "dma",
		"amoled-init", "amoled-clear", "brightness", "i2c",
		"touch-reset", "touch-probe", "touch-restore", "lv-init",
		"display-create", "buffers", "events", "indev", "flush-irq",
		"touch-irq", "done", "ui-update", "lv-timer"
	};

	if (Stage >= (sizeof(Names) / sizeof(Names[0])))
		return "?";

	return Names[Stage];
}


/***************************************************************************************/
/* Wait for the next TE edge without drawing, to keep the loop on the scan's
   cadence when a frame had nothing to draw. Not counted in the flush's TE
   statistics. If TE never arrived, fall back to a frame's worth of sleep so
   the loop cannot spin. */
void Panel_WaitFrame(void)
{
	uint32_t Seen = TeEdges;
	uint32_t Start = time_us_32();

	FrameIdled = true;

	if (!TeSyncEnabled)
	{
		sleep_ms(17);
		return;
	}

	while (TeEdges == Seen)
	{
		if ((uint32_t)(time_us_32() - Start) > PANEL_TE_TIMEOUT_US)
			return;
		tight_loop_contents();
	}
}


/***************************************************************************************/
/* Convert one run of paletted pixels into RGB565 for the wire.
 *
 * A byte load, a table lookup and a halfword store per pixel. The palette
 * entries are already in the panel's byte order (see ui_draw.c), so there is
 * no swapping here and nothing in this loop knows what a colour is.
 *
 * Unrolled by four. The loop is memory-bound on a table that fits in a few
 * cache lines, and cutting the loop overhead to a quarter is the one easy
 * thing available; anything further belongs in assembly, and only if the
 * measurement in PanelPush_t says the CPU is the limiter. */
static uint16_t *Panel_ConvertSpan(const uint8_t *S, uint32_t Count, uint16_t *Dst,
                                   const uint16_t *Palette)
{
	while (Count >= 4u)
	{
		Dst[0] = Palette[S[0]];
		Dst[1] = Palette[S[1]];
		Dst[2] = Palette[S[2]];
		Dst[3] = Palette[S[3]];
		Dst += 4;
		S += 4;
		Count -= 4u;
	}

	while (Count-- != 0u)
		*Dst++ = Palette[*S++];

	return Dst;
}


/***************************************************************************************/
/* Convert a chunk of rows, each assembled from the spans its row function
   hands back. The spans of a row must add up to the rectangle's width; a row
   function that gets that wrong is caught here rather than allowed to shift
   every pixel after it. */
static void Panel_ConvertRows(const PanelSource_t *Source, int32_t Y, int32_t X1,
                              uint32_t Width, uint32_t Lines, uint16_t *Dst)
{
	uint32_t Line;

	for (Line = 0; Line < Lines; Line++)
	{
		PanelSpan_t Spans[PANEL_MAX_SPANS];
		uint32_t Count = Source->Row(Source->Context, Y + (int32_t)Line, X1, Width, Spans);
		uint32_t Total = 0, i;

		for (i = 0; i < Count && i < PANEL_MAX_SPANS; i++)
		{
			uint32_t N = Spans[i].Count;

			if (Total + N > Width)
				N = Width - Total;
			Dst = Panel_ConvertSpan(Spans[i].Src, N, Dst, Source->Palette);
			Total += N;
		}

		if (Total < Width)
		{
			/* Short row: black out the rest, and count it - it is a bug in
			   whoever composed the row, not a panel fault. */
			RowShortfalls++;
			while (Total++ < Width)
				*Dst++ = 0u;
		}
	}
}


/***************************************************************************************/
/* Wait for the DMA to finish the chunk it is on, and say whether it had
   already finished when we got here - which means the bus went idle waiting
   for the CPU, and the conversion is the limiter rather than the wire. */
static uint32_t Panel_ChunkWait(void)
{
	uint32_t Start = time_us_32();

	if (!dma_channel_is_busy(dma_tx))
	{
		PushStarved++;
		return 0;
	}

	while (dma_channel_is_busy(dma_tx))
		tight_loop_contents();

	return (uint32_t)(time_us_32() - Start);
}


/***************************************************************************************/
/* Raise chip select, once the PIO has actually finished with the data.
 *
 * A finished DMA means the last byte reached the PIO FIFO, not that it has
 * been clocked out. Four words of FIFO and one in the output shift register
 * can still be pending, so raising chip select on the DMA's completion - which
 * is what the vendor's handler does - cuts the final pixels off every
 * rectangle sent. Small enough to miss in a demo that repaints continuously;
 * not small enough here, where nothing repaints what has not changed.
 *
 * WAIT ON FSTAT, NOT ON TXSTALL. TXSTALL reads like the exact condition - the
 * state machine sets it when an autopull finds the FIFO empty - and using it
 * cost two bugs. Clearing it before the transfer was the first: an 8-bit DMA
 * cannot keep this state machine fed, measured at 33 MB/s against a 50 MB/s
 * PIO, so it stalls repeatedly mid-transfer and sets the flag long before the
 * last byte. Clearing it here and reading it straight back was the second:
 * that is a posted peripheral write followed by a read of the same register,
 * and a clear that has not landed yet reads back as still set. Either way the
 * wait passes on a stale flag and the truncation it exists to prevent still
 * happens.
 *
 * FSTAT is live status, not a sticky flag, so there is nothing to clear and
 * nothing to race. */
static void Panel_EndTransfer(void)
{
	uint32_t Spins = 0;

	while (!pio_sm_is_tx_fifo_empty(qspi.pio, qspi.sm))
	{
		if (++Spins > PANEL_DRAIN_SPINS)
		{
			/* Should never happen. Counted rather than ignored because the
			   alternative reading - a wedged PIO - would otherwise show up
			   only as a display that has stopped updating. */
			FlushTimeouts++;
			break;
		}
	}

	/* The FIFO is empty; at most one byte remains in the output shift
	   register, which is two nibble clocks - four system cycles at this
	   divider. A microsecond is a hundred times that. */
	busy_wait_us_32(1);

	QSPI_Deselect(qspi);
	Flushes++;

	/* Measured to here, not to the end of the DMA: the bus is held until chip
	   select rises, and the drain above is part of holding it. */
	FlushBusyUs += time_us_32() - FlushStartUs;
	if (Spins > DrainSpinsMax)
		DrainSpinsMax = Spins;
}


/***************************************************************************************/
/* Start a chunk's DMA now. */
static void Panel_StartChunk(const uint16_t *Data, uint32_t Bytes)
{
	dma_channel_configure(dma_tx, &c, &qspi.pio->txf[qspi.sm], Data, Bytes, true);
}


/***************************************************************************************/
/* Send one rectangle, optionally starting on the panel's next TE pulse.
 *
 * ONE WINDOW, CHIP SELECT HELD, N CHUNKS.
 *
 * The window command and the 0x2C that follows it open a single pixel stream;
 * the panel takes as many pixels as the window holds, and does not care how
 * they are grouped on the wire. So the chunks are not separate transfers to
 * the panel - they are one transfer the DMA is fed in pieces, which is what
 * allows the conversion of chunk N+1 to overlap the transmission of chunk N.
 *
 * If the CPU falls behind, the PIO stalls with chip select still low and the
 * rectangle simply takes longer: a stall is not a corruption. The failure that
 * WOULD corrupt is losing chip select between chunks, which is why it is
 * raised in exactly one place, after the last chunk has drained.
 *
 * With AtTe, the first chunk is converted BEFORE the wait and its DMA started
 * by the TE interrupt itself - see Panel_GpioIrq() - so the pixels begin
 * moving within interrupt latency of the pulse.
 */
static void Panel_SendRect(const PanelSource_t *Source, int32_t X1, int32_t Y1,
                           int32_t X2, int32_t Y2, bool AtTe)
{
	uint32_t Width, Height, Line, Slot, Lines;
	uint32_t Start;

	Panel_RoundArea(&X1, &Y1, &X2, &Y2);
	Width = (uint32_t)(X2 - X1 + 1);
	Height = (uint32_t)(Y2 - Y1 + 1);

	if (X2 < X1 || Y2 < Y1 || Width > (uint32_t)PANEL_WIDTH)
		return;

	/* Both tests, because they fail differently. A busy channel means a push
	   was started while the last one was still moving. Chip select still low
	   means the previous push never ended - which would put these pixels into
	   the previous window. */
	if (dma_channel_is_busy(dma_tx))
		FlushOverlaps++;
	if (gpio_get(qspi.pin_cs) == 0)
		FlushCsOverlaps++;

	Lines = (Height < PANEL_CHUNK_LINES) ? Height : PANEL_CHUNK_LINES;
	Start = time_us_32();
	Panel_ConvertRows(Source, Y1, X1, Width, Lines, ChunkBuf[0]);
	SendConvertUs += (uint32_t)(time_us_32() - Start);

	/* The window, and the pixel-write command that opens the stream. Their
	   SetWindows takes an exclusive end; ours are inclusive. */
	AMOLED_1IN75_SetWindows((uint32_t)X1, (uint32_t)Y1,
	                        (uint32_t)X2 + 1u, (uint32_t)Y2 + 1u);
	QSPI_Select(qspi);
	QSPI_Pixel_Write(qspi, 0x2C);

	/* The vendor's own init sets this dreq to the receive direction, which is
	   wrong; every one of their transmit paths quietly overrides it on the way
	   past. Set it correctly here too rather than depending on that. */
	channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

	PushArmData = ChunkBuf[0];
	PushArmBytes = Width * Lines * 2u;

	if (AtTe && TeSyncEnabled)
	{
		uint32_t Seen = TeEdges;

		Start = time_us_32();
		PushArmed = true;

		while (PushArmed)
		{
			if ((uint32_t)(time_us_32() - Start) > PANEL_TE_TIMEOUT_US)
			{
				/* The pulse can arrive between the test above and here, and
				   the handler would then already have started the transfer.
				   Starting it again would restart a running DMA from the top
				   of the buffer, so check the flag rather than the clock. */
				if (!PushArmed)
					break;

				/* No pulse. Disarm and send it now rather than drop the frame:
				   a panel that has stopped pulsing TE should still show
				   something, and Panel_Te() reports the timeout. */
				PushArmed = false;
				TeSyncTimeouts++;
				if (TeEdges == 0u && TeSyncTimeouts >= PANEL_TE_GIVE_UP)
					TeSyncEnabled = false;
				PushStartUs = time_us_32();
				FlushStartUs = PushStartUs;
				Panel_StartChunk(PushArmData, PushArmBytes);
				break;
			}
			tight_loop_contents();
		}

		if (TeEdges != Seen)
		{
			TeSyncWaits++;
			TeWaitUs += (uint32_t)(PushStartUs - Start);

			/* Two or more scans since the last push is a scan missed - unless
			   a frame in between chose not to draw. */
			if (!FrameIdled && TeLastPushEdge != 0u
			    && (uint32_t)(TeEdges - TeLastPushEdge) >= 2u)
				TeLateFrames++;
			TeLastPushEdge = TeEdges;
			FrameIdled = false;
		}
	}
	else
	{
		FlushStartUs = time_us_32();
		if (AtTe)
			PushStartUs = FlushStartUs;
		Panel_StartChunk(PushArmData, PushArmBytes);
	}

	FlushBytes += PushArmBytes;
	PushChunks++;

	/* Convert, wait, hand over. The wait is after the conversion, so the
	   conversion happens while the wire is busy - which is the whole point. */
	Line = Lines;
	Slot = 1u;

	while (Line < Height)
	{
		uint32_t Bytes;

		Lines = ((Height - Line) < PANEL_CHUNK_LINES) ? (Height - Line)
		                                              : PANEL_CHUNK_LINES;
		Bytes = Width * Lines * 2u;

		Start = time_us_32();
		Panel_ConvertRows(Source, Y1 + (int32_t)Line, X1, Width, Lines, ChunkBuf[Slot]);
		SendConvertUs += (uint32_t)(time_us_32() - Start);

		SendBlockedUs += Panel_ChunkWait();
		Panel_StartChunk(ChunkBuf[Slot], Bytes);

		FlushBytes += Bytes;
		PushChunks++;
		Slot ^= 1u;
		Line += Lines;
	}

	SendBlockedUs += Panel_ChunkWait();
	Panel_EndTransfer();
	RenderTotalPx += Width * Height;
}


/***************************************************************************************/
/* The frame's bookkeeping around one or more rectangles sent from one TE. */
static void Panel_FrameBegin(void)
{
	SendConvertUs = 0;
	SendBlockedUs = 0;
}

static void Panel_FrameEnd(void)
{
	PushLastConvertUs = SendConvertUs;
	PushLastBlockedUs = SendBlockedUs;
	PushLastTotalUs = (uint32_t)(time_us_32() - PushStartUs);
	PushFrames++;
	RenderTotalUs += PushLastTotalUs;
}


/***************************************************************************************/
/* The row function for a plain buffer: one span per row. */
typedef struct
{
	const uint8_t *Src;
	uint32_t Stride;
} PanelBuffer_t;

static uint32_t Panel_BufferRow(void *Context, int32_t Y, int32_t X1, uint32_t Width,
                                PanelSpan_t *Spans)
{
	const PanelBuffer_t *B = (const PanelBuffer_t *)Context;

	Spans[0].Src = B->Src + ((uint32_t)Y * B->Stride) + (uint32_t)X1;
	Spans[0].Count = Width;
	return 1u;
}


/***************************************************************************************/
void Panel_PushPaletted(const uint8_t *Src, uint32_t SrcStride,
                        const uint16_t *Palette,
                        int32_t X1, int32_t Y1, int32_t X2, int32_t Y2)
{
	PanelRect_t R;

	R.X1 = X1;
	R.Y1 = Y1;
	R.X2 = X2;
	R.Y2 = Y2;
	Panel_PushRegions(Src, SrcStride, Palette, &R, 1u);
}


/***************************************************************************************/
/* SEVERAL RECTANGLES, ONE FRAME.
 *
 * The first waits for TE; the rest follow it immediately, in order down the
 * screen. That order is what keeps them tear-free: the scan also runs down the
 * screen from the pulse, and a few small rectangles are sent in well under the
 * time the scan takes to reach the rows below the first. Each has a window and
 * a chip select of its own - a pixel stream fills one rectangle only. */
void Panel_PushRegions(const uint8_t *Src, uint32_t SrcStride, const uint16_t *Palette,
                       const PanelRect_t *Regions, uint32_t Count)
{
	PanelBuffer_t Buffer;
	PanelSource_t Source;
	PanelRect_t Sorted[PANEL_MAX_REGIONS];
	uint32_t i, j, n;

	if (Count == 0u)
		return;

	n = (Count < PANEL_MAX_REGIONS) ? Count : PANEL_MAX_REGIONS;
	for (i = 0; i < n; i++)
	{
		PanelRect_t R = Regions[i];

		for (j = i; j > 0u && Sorted[j - 1u].Y1 > R.Y1; j--)
			Sorted[j] = Sorted[j - 1u];
		Sorted[j] = R;
	}

	Buffer.Src = Src;
	Buffer.Stride = SrcStride;
	Source.Row = Panel_BufferRow;
	Source.Context = &Buffer;
	Source.Palette = Palette;

	Panel_FrameBegin();
	for (i = 0; i < n; i++)
		Panel_SendRect(&Source, Sorted[i].X1, Sorted[i].Y1, Sorted[i].X2, Sorted[i].Y2,
		               i == 0u);
	PushRegionsLast = n;
	Panel_FrameEnd();
}


/***************************************************************************************/
void Panel_PushRows(const PanelSource_t *Source,
                    int32_t X1, int32_t Y1, int32_t X2, int32_t Y2)
{
	Panel_FrameBegin();
	Panel_SendRect(Source, X1, Y1, X2, Y2, true);
	PushRegionsLast = 1u;
	Panel_FrameEnd();
}


/***************************************************************************************/
void Panel_Push(PanelPush_t *Out)
{
	Out->Frames = PushFrames;
	Out->Chunks = PushChunks;
	Out->Starved = PushStarved;
	Out->LastConvertUs = PushLastConvertUs;
	Out->LastBlockedUs = PushLastBlockedUs;
	Out->LastTotalUs = PushLastTotalUs;
	Out->LastRegions = PushRegionsLast;
	Out->RowShortfalls = RowShortfalls;
}


/***************************************************************************************/
void Panel_SetBrightness(uint8_t Percent)
{
	AMOLED_1IN75_SetBrightness(Percent);
}


/***************************************************************************************/
uint32_t Panel_Flushes(void)		{ return Flushes; }


void Panel_Te(PanelTe_t *Out)
{
	Out->Edges = TeEdges;
	Out->PeriodUs = TePeriodUs;
	Out->Enabled = TeSyncEnabled;
	Out->Waits = TeSyncWaits;
	Out->Timeouts = TeSyncTimeouts;
	Out->AvgWaitUs = (TeSyncWaits == 0u) ? 0u : (uint32_t)(TeWaitUs / TeSyncWaits);
	Out->LateFrames = TeLateFrames;
}


uint64_t Panel_RenderTotalUs(void)	{ return RenderTotalUs; }
uint64_t Panel_RenderTotalPx(void)	{ return RenderTotalPx; }
uint32_t Panel_DrainSpinsMax(void)	{ return DrainSpinsMax; }
uint32_t Panel_RoundedAreas(void)	{ return RoundedAreas; }
uint32_t Panel_FlushOverlaps(void)	{ return FlushOverlaps; }
uint32_t Panel_FlushCsOverlaps(void)	{ return FlushCsOverlaps; }


/* Tenths of a megabyte per second, over every flush since boot. Integer
   throughout: a byte per microsecond is a megabyte per second, so this is just
   the ratio scaled by ten. */
uint32_t Panel_FlushMbPerSx10(void)
{
	if (FlushBusyUs == 0u)
		return 0;

	return (uint32_t)((FlushBytes * 10u) / FlushBusyUs);
}


/* Microseconds of bus time per rendered frame - the figure that matters for
   M4, since what competes with can2040 is the share of wall-clock time the
   panel spends mid-burst rather than the frame rate. */
uint32_t Panel_FlushBusyUsPerFrame(void)
{
	if (PushFrames == 0u)
		return 0;

	return (uint32_t)(FlushBusyUs / PushFrames);
}
uint32_t Panel_FlushTimeouts(void)	{ return FlushTimeouts; }
bool Panel_TouchPresent(void)		{ return TouchPresent; }
uint16_t Panel_TouchChipType(void)	{ return TouchChipType; }
uint32_t Panel_TouchReports(void)	{ return TouchReports; }
uint32_t Panel_TouchRiseEdges(void)	{ return TouchRiseEdges; }
uint32_t Panel_TouchFallEdges(void)	{ return TouchFallEdges; }
uint32_t Panel_TouchPresses(void)	{ return TouchPresses; }

void Panel_TouchLast(uint16_t *X, uint16_t *Y)
{
	*X = (uint16_t)TouchX;
	*Y = (uint16_t)TouchY;
}
