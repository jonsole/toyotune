/*
 * panel.c
 *
 * The CO5300 AMOLED and the CST9217 touch controller, bound to LVGL.
 *
 * The transport, the panel init sequence and the touch register map all come
 * from Waveshare's drivers in firmware/vendor/ - see vendor/README.md for
 * provenance. Those files are kept as delivered so the diff against a future
 * vendor release stays readable, which means the places where their example
 * is wrong are corrected here instead of there. Three of them:
 *
 *  1. THE DRAW BUFFER. Their example allocates
 *     malloc(DISP_HOR_RES * DISP_VER_RES) - a count in bytes - and tells LVGL
 *     that number of PIXELS, so LVGL believes it has twice the memory it was
 *     given: a 212 KB overflow. The buffers here are static lv_color_t arrays
 *     and the count handed to LVGL is the array length, so the unit cannot
 *     drift from the allocation. They are also partial rather than
 *     full-screen, because no PSRAM is fitted (measured - see
 *     test/psram_probe.c) and a 424 KB framebuffer does not fit.
 *
 *  2. CHIP SELECT AFTER THE DMA. Their completion handler raises chip select
 *     as soon as the DMA finishes, but a finished DMA only means the last byte
 *     reached the PIO FIFO - not that it has been clocked out. See the drain
 *     in Panel_FlushDoneIrq().
 *
 *  3. TOUCH AS AN INTERRUPT. Their handler latches a press per interrupt and
 *     releases it on the next read, which LVGL can only ever read as a tap.
 *     Swiping between faces needs the press held while the finger moves, so
 *     this polls. See Panel_TouchRead().
 *
 * Everything here runs on core 1 except Panel_ClockInit(). LVGL is not
 * thread-safe and nothing in this file may be called from core 0.
 */

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/i2c.h"
#include "hardware/irq.h"
#include "hardware/pio.h"
#include "hardware/structs/pio.h"

#include "lvgl.h"

/* Vendor. DEV_Config.h carries the pin map and declares the globals dma_tx
   and c - the DMA channel and its config - that their panel driver transmits
   through. The name `c` for a global is theirs; do not shadow it here. */
#include "DEV_Config.h"
#include "AMOLED_1in75.h"
#include "CST9217.h"
#include "qspi_pio.h"

#include "panel.h"

/* The panel takes RGB565 high byte first and the flush hands LVGL's buffer
   straight to DMA, so LVGL has to render it pre-swapped. Wrong and the gauges
   simply come out in wrong colours - no error, no clue - which is worth an
   #error rather than a comment.

   v8 arranged this with LV_COLOR_16_SWAP. v9 has no such option: the byte
   order is a property of the display's colour format, set below with
   lv_display_set_color_format(), and it only works if the software blender
   was built with the swapped format compiled in. */
#if LV_COLOR_DEPTH != 16
#error "The CO5300 flush path assumes RGB565 - set LV_COLOR_DEPTH 16 in lv_conf.h"
#endif
#if LV_DRAW_SW_SUPPORT_RGB565_SWAPPED != 1
#error "The CO5300 wants the high byte first - LV_DRAW_SW_SUPPORT_RGB565_SWAPPED must be 1"
#endif

/* Draw buffer height, in whole display lines - and why there is one buffer.
 *
 * The needle tore because a frame reached the panel in pieces. The panel scans
 * out of its own memory at its own rate, and a needle update used to arrive as
 * 60-line bands spread over ~45 ms, so a scan could land between two bands and
 * show half an old needle beside half a new one.
 *
 * So the buffer is sized to take a whole ordinary frame at once. LVGL renders
 * an invalid area into it completely and then flushes it, which makes each
 * frame one ~1 ms DMA burst rather than a write smeared across many scans. That
 * only fits because the gauge needle now invalidates just the region it swept
 * (UiLvgl_SetNeedle() in ui_lvgl.c) instead of a square from the corner of the
 * dial - 100 lines of 466 is 46,600 pixels, comfortably above a needle frame.
 * A full-screen redraw, a page change, still goes out in bands; that is a
 * crossfade and does not show a tear the way a moving edge does.
 *
 * One buffer, not two. A second buffer only paid for LVGL rendering the next
 * band while the previous one was on the wire, and a 1 ms transfer leaves
 * nothing worth overlapping. 93 KB, against 110 KB for the old pair.
 *
 * On its own that narrows the tear window from ~42 ms to ~1 ms without closing
 * it, since a scan can still cross a 1 ms burst. Panel_WaitForTe() closes it,
 * by starting each burst at the panel's TE pulse. Reading the scan position
 * from register 45h was tried first and does not work on this board - see
 * vendor/README.md finding 8. */
#define PANEL_BUF_LINES		(100)
#define PANEL_BUF_PIXELS	((uint32_t)PANEL_WIDTH * (uint32_t)PANEL_BUF_LINES)
#define PANEL_BUF_BYTES		(PANEL_BUF_PIXELS * 2u)

/* Bound on the PIO drain below. The FIFO holds four words plus one in the
   output shift register - at most 40 nibbles, around 400 ns - so this is
   roughly a hundred times the time it should ever need. Bounded rather than
   open because this spins inside an interrupt, and a bus drain that can hang
   forever is a fault this repo has been bitten by before. */
#define PANEL_DRAIN_SPINS	(1000u)

#define PANEL_DEFAULT_BRIGHTNESS	(80u)

/* The panel's tearing-effect output. Not in Waveshare's pin map or sources at
   all - GPIO17 comes from the board schematic. */
#define PANEL_TE_PIN		(17u)

/* How long a flush waits for a TE pulse before giving up on it. A 60 Hz frame
   is 16.7 ms, so this covers one missed pulse with room to spare. */
#define PANEL_TE_TIMEOUT_US	(40000u)

/* Timeouts with no TE edge ever seen before sync is abandoned. A panel that
   never pulses TE would otherwise cost every single frame the full timeout. */
#define PANEL_TE_GIVE_UP	(5u)

static lv_display_t	       *Disp;
static lv_indev_t	       *Touch;

/* Byte arrays, not lv_color_t. In v9 lv_color_t is a 24-bit RGB888 struct and
   is no longer the framebuffer pixel type - the pixel format belongs to the
   display, so a buffer is just bytes. lv_display_set_buffers() takes its size
   in bytes too, unlike v8's lv_disp_draw_buf_init() which took pixels, so
   sizeof() is the right thing to pass and there is no unit left to confuse.
   That confusion is exactly what broke the vendor example (vendor/README.md). */
static uint8_t			DrawBuf[PANEL_BUF_BYTES];

/* Last known touch position, held across releases - see Panel_TouchRead(). */
static int32_t			TouchX;
static int32_t			TouchY;

static uint32_t			Flushes;
static uint32_t			FlushTimeouts;

/* Filled in by LVGL after every refresh. RefreshMaxMs is the one that matters:
   a page transition invalidates the whole screen, so the worst refresh is a
   full-screen one and its reciprocal is the frame rate a slide actually gets. */
static uint32_t			RefreshLastMs;
static uint32_t			RefreshMaxMs;
static uint32_t			RenderStartMs;

/* Cumulative render time and pixels over every refresh that drew something,
   for benchmarking: diff two snapshots to get throughput over a window.
   Microseconds, because a millisecond tick quantises a 45 ms frame by 2%. */
static uint32_t			RenderStartUs;
static uint64_t			RenderTotalUs;
static uint64_t			RenderTotalPx;
static uint32_t			RefreshLastPx;
static uint32_t			Refreshes;

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
   final report was missed. Short enough not to leave a phantom press behind -
   which LVGL reads as a finger still on the glass, and is exactly what made
   the first working display have a dead touch panel. */
#define TOUCH_HOLD_TIMEOUT_MS	(80u)
static bool			TouchPresent;
static uint16_t			TouchChipType;


/***************************************************************************************/
/* LVGL's millisecond tick. v8 took this as an expression in lv_conf.h; v9 asks
   for a callback, which is tidier - LVGL no longer needs the SDK headers on
   its include path just to know the time. */
static uint32_t Panel_TickMs(void)
{
	return to_ms_since_boot(get_absolute_time());
}


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
/* Expand an invalidated area to what the panel can actually address.
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
 * This was disp_drv.rounder_cb in v8. v9 deleted that callback and hands the
 * area out through LV_EVENT_INVALIDATE_AREA instead; the arithmetic is
 * untouched, and LVGL still probes it to pick a band height whose ROUNDED
 * height fits the draw buffer.
 *
 * Start down to even and end up to odd also makes every width and height even.
 * Both clamps land on values that already have the right parity - 0 is even,
 * 465 is odd - so clamping cannot put the alignment back. */
static void Panel_InvalidateArea(lv_event_t *Event)
{
	lv_area_t *Area = lv_event_get_invalidated_area(Event);
	bool Needed;

	Needed = ((Area->x1 & 1) != 0) || ((Area->x2 & 1) == 0)
	         || ((Area->y1 & 1) != 0) || ((Area->y2 & 1) == 0);

	if ((Area->x1 & 1) != 0)
		Area->x1 = (int32_t)(Area->x1 - 1);
	if ((Area->x2 & 1) == 0)
		Area->x2 = (int32_t)(Area->x2 + 1);

	if ((Area->y1 & 1) != 0)
		Area->y1 = (int32_t)(Area->y1 - 1);
	if ((Area->y2 & 1) == 0)
		Area->y2 = (int32_t)(Area->y2 + 1);

	if (Area->x1 < 0)
		Area->x1 = 0;
	if (Area->x2 > (int32_t)(PANEL_WIDTH - 1))
		Area->x2 = (int32_t)(PANEL_WIDTH - 1);
	if (Area->y1 < 0)
		Area->y1 = 0;
	if (Area->y2 > (int32_t)(PANEL_HEIGHT - 1))
		Area->y2 = (int32_t)(PANEL_HEIGHT - 1);

	/* Counts LVGL's own probe calls from get_max_row() as well as real areas,
	   so treat it as "is this firing at all" rather than as a frame count. */
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
static void Panel_WaitForTe(void)
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
/* Called by LVGL when a rectangle is ready to go to the glass. Starts the DMA
   and returns immediately; completion is reported from the interrupt below. */
static void Panel_Flush(lv_display_t *Display, const lv_area_t *Area,
                        uint8_t *Pixels)
{
	uint32_t Bytes;

	(void)Display;

	/* Both tests, because they fail differently. A busy channel means LVGL
	   handed us a second area while the first was still moving. Chip select
	   still low means the channel finished but our completion handler has not
	   run, so the PIO may not have drained and CS was never raised. */
	if (dma_channel_is_busy(dma_tx))
		FlushOverlaps++;
	if (gpio_get(qspi.pin_cs) == 0)
		FlushCsOverlaps++;

	/* Two bytes a pixel, from the display's own colour format rather than a
	   literal - this is the multiplication the vendor example got wrong, and
	   deriving it means a format change cannot silently halve it. */
	Bytes = (uint32_t)lv_area_get_width(Area)
	        * (uint32_t)lv_area_get_height(Area)
	        * (uint32_t)lv_color_format_get_size(lv_display_get_color_format(Disp));

	/* Start the write in vertical blanking, so the scan cannot show these
	   rows half written. See Panel_WaitForTe(). */
	Panel_WaitForTe();

	/* LVGL's area bounds are inclusive; the panel's window registers are not. */
	AMOLED_1IN75_SetWindows((uint32_t)Area->x1, (uint32_t)Area->y1,
	                        (uint32_t)Area->x2 + 1u, (uint32_t)Area->y2 + 1u);

	QSPI_Select(qspi);
	QSPI_Pixel_Write(qspi, 0x2C);

	/* The vendor's own init sets this dreq to the receive direction, which is
	   wrong; every one of their transmit paths quietly overrides it on the way
	   past. Set it correctly here too rather than depending on that. */
	channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

	FlushBytes += Bytes;
	RefreshLastPx += (uint32_t)lv_area_get_width(Area)
	                 * (uint32_t)lv_area_get_height(Area);
	FlushStartUs = time_us_32();

	/* DMA_SIZE_8, so the count is in bytes. Byte writes to a PIO TX FIFO are
	   replicated across the word, which is what lets an 8-bit DMA feed a
	   program whose autopull threshold is 8 and which shifts out of the top of
	   the register. */
	dma_channel_configure(dma_tx, &c, &qspi.pio->txf[qspi.sm], Pixels, Bytes,
	                      true);

	/* No wait. lv_disp_flush_ready() comes from the interrupt, so LVGL draws
	   into the other buffer while this one is still on the wire. */
}


/***************************************************************************************/
/* Refresh timing: how long a redraw took and how many pixels it covered.
   v8 handed both to a monitor_cb; v9 has no such callback, so the time is
   taken between LV_EVENT_RENDER_START and LV_EVENT_RENDER_READY and the pixel
   count is accumulated by the flush. Reported over the serial console, which
   is why this exists alongside the on-screen LV_USE_PERF_MONITOR - the console
   figures can be read without a camera pointed at the gauge. */
static void Panel_RenderStart(lv_event_t *Event)
{
	(void)Event;

	RenderStartMs = lv_tick_get();
	RenderStartUs = time_us_32();
	RefreshLastPx = 0;
}


/***************************************************************************************/
static void Panel_RenderReady(lv_event_t *Event)
{
	uint32_t TimeMs = lv_tick_elaps(RenderStartMs);

	(void)Event;

	/* Ignore refresh cycles that drew nothing. v9 sends these events even when
	   there was no invalid area, and counting them would bury the real figures
	   under zeroes - which matters here because unchanged widgets are never
	   repainted, so most cycles draw nothing at all. */
	if (RefreshLastPx == 0u)
		return;

	RefreshLastMs = TimeMs;
	Refreshes++;
	RenderTotalUs += (uint32_t)(time_us_32() - RenderStartUs);
	RenderTotalPx += RefreshLastPx;

	if (TimeMs > RefreshMaxMs)
		RefreshMaxMs = TimeMs;
}


/***************************************************************************************/
static void Panel_FlushDoneIrq(void)
{
	uint32_t Spins = 0;

	if (!dma_channel_get_irq0_status(dma_tx))
		return;

	dma_channel_acknowledge_irq0(dma_tx);

	/* A finished DMA means the last byte reached the PIO FIFO, not that it has
	   been clocked out of it. Four words of FIFO and one in the output shift
	   register can still be pending, so raising chip select here - which is
	   what the vendor's handler does - cuts the final pixels off every flushed
	   rectangle. Small enough to miss in a demo that repaints the whole screen
	   continuously; not small enough here, where nothing repaints a face that
	   has not changed, so truncated pixels persist and accumulate.

	   WAIT ON FSTAT, NOT ON TXSTALL.
	   TXSTALL reads like the exact condition - the state machine sets it when
	   an autopull finds the FIFO empty - and using it cost two bugs. Clearing
	   it before the transfer was the first: the 8-bit DMA cannot keep this
	   state machine fed, measured at 33 MB/s against a 50 MB/s PIO, so it
	   stalls repeatedly mid-transfer and sets the flag long before the last
	   byte. Clearing it here and reading it straight back was the second: that
	   is a posted peripheral write followed by a read of the same register, and
	   a clear that has not landed yet reads back as still set. Either way the
	   wait passes immediately on a stale flag and the truncation it exists to
	   prevent still happens.

	   FSTAT is live status, not a sticky flag, so there is nothing to clear and
	   nothing to race. */
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
	   divider. A microsecond is a hundred times that, and at eight chunks a
	   frame it costs eight microseconds of a twenty-six millisecond frame. Not
	   worth being clever about. */
	busy_wait_us_32(1);

	QSPI_Deselect(qspi);
	Flushes++;

	/* Measured to here, not to the end of the DMA: the bus is held until chip
	   select rises, and the drain above is part of holding it. */
	FlushBusyUs += time_us_32() - FlushStartUs;
	if (Spins > DrainSpinsMax)
		DrainSpinsMax = Spins;

	lv_display_flush_ready(Disp);
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
 * at all - a stuck press is a finger LVGL believes is still down.
 */
static void Panel_TouchRead(lv_indev_t *Indev, lv_indev_data_t *Data)
{
	uint32_t NowMs = to_ms_since_boot(get_absolute_time());

	(void)Indev;

	if (TouchIntPending)
	{
		TouchIntPending = false;

		/* False is not an I2C error: their read returns false when byte 6 of
		   the reply is not 0xAB, which marks a touch report. Whether the
		   controller is on the bus at all was settled at init by
		   Panel_TouchProbe(). */
		if (CST9217_Read_Data())
		{
			TouchReports++;

			if (CST9217.points > 0)
			{
				/* Their driver already maps the reading to display
				   coordinates, including the 180 degree flip - but 466 - 0 is
				   466 and the last valid pixel is 465, so clamp rather than
				   hand LVGL a point one off the edge. */
				uint16_t X = CST9217.data[0].x;
				uint16_t Y = CST9217.data[0].y;

				if (X >= PANEL_WIDTH)
					X = PANEL_WIDTH - 1u;
				if (Y >= PANEL_HEIGHT)
					Y = PANEL_HEIGHT - 1u;

				TouchX = (int32_t)X;
				TouchY = (int32_t)Y;
				TouchPresses++;
				TouchHeld = true;
				TouchLastReportMs = NowMs;
			}
			else
			{
				TouchHeld = false;
			}
		}
	}
	else if (TouchHeld && (NowMs - TouchLastReportMs) > TOUCH_HOLD_TIMEOUT_MS)
	{
		TouchHeld = false;
	}

	Data->state = TouchHeld ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;

	/* The last known position travels with a release too: LVGL reads the point
	   on the releasing call to work out where the gesture ended, and (0,0)
	   there would look like a sudden drag to the corner. */
	Data->point.x = TouchX;
	Data->point.y = TouchY;
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

	/* ---- LVGL --------------------------------------------------------- */

	lv_init();

	/* AFTER lv_init(), not before. lv_init() resets LVGL's global state, which
	   includes the tick callback - registering it first looks tidier and is
	   silently undone, leaving lv_tick_get() stuck at zero. LVGL does say so,
	   with "It seems lv_tick_inc() is not called" out of lv_timer_handler, but
	   that warning goes to a console nobody is attached to yet. Everything
	   downstream then measures zero: no animation advances, no timer fires on
	   schedule, and every render time reads 0 ms. */
	lv_tick_set_cb(Panel_TickMs);
	InitStage = PANEL_STAGE_LV_INIT;

	Disp = lv_display_create(PANEL_WIDTH, PANEL_HEIGHT);
	InitStage = PANEL_STAGE_DISPLAY_CREATE;

	/* Pre-swapped RGB565: the panel wants the high byte first and the flush
	   hands the buffer straight to DMA, so the renderer produces that order
	   itself rather than a pass being made over every buffer. See the note at
	   the top of this file. */
	lv_display_set_color_format(Disp, LV_COLOR_FORMAT_RGB565_SWAPPED);

	/* Size in BYTES in v9, where v8 wanted pixels - and sizeof() is the whole
	   array, so the two cannot disagree. */
	lv_display_set_buffers(Disp, DrawBuf, NULL, sizeof(DrawBuf),
	                       LV_DISPLAY_RENDER_MODE_PARTIAL);
	InitStage = PANEL_STAGE_BUFFERS;

	lv_display_set_flush_cb(Disp, Panel_Flush);

	/* The window alignment the CO5300 requires, and the refresh timing, are
	   events in v9 rather than driver callbacks. */
	lv_display_add_event_cb(Disp, Panel_InvalidateArea,
	                        LV_EVENT_INVALIDATE_AREA, NULL);
	lv_display_add_event_cb(Disp, Panel_RenderStart,
	                        LV_EVENT_RENDER_START, NULL);
	lv_display_add_event_cb(Disp, Panel_RenderReady,
	                        LV_EVENT_RENDER_READY, NULL);
	InitStage = PANEL_STAGE_EVENTS;

	Touch = lv_indev_create();
	lv_indev_set_type(Touch, LV_INDEV_TYPE_POINTER);
	lv_indev_set_read_cb(Touch, Panel_TouchRead);
	InitStage = PANEL_STAGE_INDEV;

	/* Start centred, so a release before any press cannot read as a gesture
	   from the corner. */
	TouchX = PANEL_WIDTH / 2;
	TouchY = PANEL_HEIGHT / 2;

	/* ---- flush completion --------------------------------------------- */

	/* Installed from core 1, which is where it must run: the vector table is
	   per core, and lv_disp_flush_ready() has to be called on the core that
	   owns LVGL. Core 0 leaves DMA_IRQ_0 alone. */
	/* Clear any latched completion first. AMOLED_1IN75_Clear() above runs 466
	   polled DMA transfers, which leave the channel's interrupt status set;
	   enabling the interrupt on top of that fires the handler immediately, for
	   a flush that never happened. Harmless in itself - LVGL is not waiting on
	   anything yet - but it calls lv_disp_flush_ready() unbidden and it
	   reported a 759 ms first flush, because the handler timed against a start
	   stamp that had never been taken. */
	dma_channel_acknowledge_irq0(dma_tx);

	dma_channel_set_irq0_enabled(dma_tx, true);
	irq_set_exclusive_handler(DMA_IRQ_0, Panel_FlushDoneIrq);
	irq_set_enabled(DMA_IRQ_0, true);
	InitStage = PANEL_STAGE_FLUSH_IRQ;

	/* Likewise core 1, and for the same reason: the flag it sets is read by
	   the LVGL callback. Only enabled once the controller is out of reset and
	   has been identified, so a reset pulse cannot be counted as a report. */
	if (TouchPresent)
		gpio_set_irq_enabled(TOUCH_INT_PIN,
		                     GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);

	/* TE. An input the panel drives, so no pull: a pull-down on a digital
	   input can latch at about 2.2 V on RP2350 (erratum E9), and nothing is
	   gained by one here. Registering it is also what installs the shared
	   callback, which is why touch above only enables its events - see
	   Panel_GpioIrq(). Order does not matter: both are enabled before LVGL
	   draws a thing. */
	gpio_init(PANEL_TE_PIN);
	gpio_set_dir(PANEL_TE_PIN, GPIO_IN);
	gpio_disable_pulls(PANEL_TE_PIN);
	gpio_set_irq_enabled_with_callback(PANEL_TE_PIN, GPIO_IRQ_EDGE_RISE,
	                                   true, Panel_GpioIrq);

	InitStage = PANEL_STAGE_TOUCH_IRQ;

	printf("panel: %dx%d, one %u-line draw buffer (%lu bytes)\n",
	       PANEL_WIDTH, PANEL_HEIGHT, (unsigned)PANEL_BUF_LINES,
	       (unsigned long)sizeof(DrawBuf));
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
uint32_t Panel_Service(void)
{
	return lv_timer_handler();
}


/***************************************************************************************/
/* The screen LVGL created for this display. ui_lvgl.c needs it to blank the
   very first frame; everything after that is its own screens. */
lv_display_t *Panel_Display(void)
{
	return Disp;
}


/***************************************************************************************/
void Panel_SetBrightness(uint8_t Percent)
{
	AMOLED_1IN75_SetBrightness(Percent);
}


/***************************************************************************************/
uint32_t Panel_Flushes(void)		{ return Flushes; }
uint32_t Panel_RefreshLastMs(void)	{ return RefreshLastMs; }
uint32_t Panel_RefreshMaxMs(void)	{ return RefreshMaxMs; }
uint32_t Panel_RefreshLastPx(void)	{ return RefreshLastPx; }
uint32_t Panel_Refreshes(void)		{ return Refreshes; }


void Panel_Te(PanelTe_t *Out)
{
	Out->Edges = TeEdges;
	Out->PeriodUs = TePeriodUs;
	Out->Enabled = TeSyncEnabled;
	Out->Waits = TeSyncWaits;
	Out->Timeouts = TeSyncTimeouts;
	Out->AvgWaitUs = (TeSyncWaits == 0u) ? 0u : (uint32_t)(TeWaitUs / TeSyncWaits);
}


/* LVGL's own heap: current and peak use, in bytes. For sizing LV_MEM_SIZE from
   a measurement rather than a guess. Core 1 only, like the rest of LVGL. */
void Panel_Heap(uint32_t *UsedBytes, uint32_t *PeakBytes, uint32_t *TotalBytes)
{
	lv_mem_monitor_t Mon;

	lv_mem_monitor(&Mon);
	*TotalBytes = (uint32_t)Mon.total_size;
	*UsedBytes = (uint32_t)(Mon.total_size - Mon.free_size);
	*PeakBytes = (uint32_t)Mon.max_used;
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
	if (Refreshes == 0u)
		return 0;

	return (uint32_t)(FlushBusyUs / Refreshes);
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
