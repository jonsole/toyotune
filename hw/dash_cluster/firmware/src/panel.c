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
   straight to DMA, so LVGL has to store it pre-swapped. Wrong here and the
   gauges simply render in wrong colours - no error, no clue. Worth an #error
   rather than a comment. */
#if LV_COLOR_DEPTH != 16
#error "The CO5300 flush path assumes RGB565 - set LV_COLOR_DEPTH 16 in lv_conf.h"
#endif
#if LV_COLOR_16_SWAP != 1
#error "The CO5300 wants the high byte first - set LV_COLOR_16_SWAP 1 in lv_conf.h"
#endif

/* Draw buffer height, in whole display lines.
 *
 * 40 lines of 466 is 37 KB a buffer, 75 KB for the pair, out of 520 KB of
 * SRAM. Two buffers rather than one because the flush is asynchronous: LVGL
 * renders into the second while the first is still going out over DMA, which
 * is the only way the draw and the transfer overlap. Bigger buffers mean
 * fewer, longer DMA bursts - which is the tradeoff M4 measures, since those
 * bursts are what compete with can2040 for bus bandwidth (PLAN.md 4.2a). */
#define PANEL_BUF_LINES		(40)
#define PANEL_BUF_PIXELS	((uint32_t)PANEL_WIDTH * (uint32_t)PANEL_BUF_LINES)

/* Bound on the PIO drain below. The FIFO holds four words plus one in the
   output shift register - at most 40 nibbles, around 400 ns - so this is
   roughly a hundred times the time it should ever need. Bounded rather than
   open because this spins inside an interrupt, and a bus drain that can hang
   forever is a fault this repo has been bitten by before. */
#define PANEL_DRAIN_SPINS	(1000u)

#define PANEL_DEFAULT_BRIGHTNESS	(80u)

static lv_disp_draw_buf_t	DrawBufDesc;
static lv_disp_drv_t		DispDrv;
static lv_indev_drv_t		TouchDrv;

/* Sized in pixels by their type, which is the point: the count passed to
   lv_disp_draw_buf_init() below is the array length, so it cannot disagree
   with the allocation the way the vendor example's does. */
static lv_color_t		DrawBuf0[PANEL_BUF_PIXELS];
static lv_color_t		DrawBuf1[PANEL_BUF_PIXELS];

/* Last known touch position, held across releases - see Panel_TouchRead(). */
static lv_coord_t		TouchX;
static lv_coord_t		TouchY;

static uint32_t			Flushes;
static uint32_t			FlushTimeouts;
static uint32_t			TouchPresses;
static bool			TouchPresent;
static uint16_t			TouchChipType;


/***************************************************************************************/
/* Which PIO state machine the panel transmits through, as a stall-flag mask.
   The vendor driver switches qspi.sm between its one-wire and four-wire state
   machines, so this is read from the struct rather than fixed. */
static inline uint32_t Panel_TxStallMask(void)
{
	return 1u << (PIO_FDEBUG_TXSTALL_LSB + qspi.sm);
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
/* Called by LVGL when a rectangle is ready to go to the glass. Starts the DMA
   and returns immediately; completion is reported from the interrupt below. */
static void Panel_Flush(lv_disp_drv_t *Drv, const lv_area_t *Area,
                        lv_color_t *Pixels)
{
	uint32_t Bytes;

	(void)Drv;

	/* sizeof(lv_color_t) rather than a literal 2, deliberately: this is the
	   multiplication the vendor example got wrong, and tying it to the type
	   means a colour-depth change cannot silently halve it. */
	Bytes = (uint32_t)lv_area_get_width(Area)
	        * (uint32_t)lv_area_get_height(Area)
	        * (uint32_t)sizeof(lv_color_t);

	/* LVGL's area bounds are inclusive; the panel's window registers are not. */
	AMOLED_1IN75_SetWindows((uint32_t)Area->x1, (uint32_t)Area->y1,
	                        (uint32_t)Area->x2 + 1u, (uint32_t)Area->y2 + 1u);

	QSPI_Select(qspi);
	QSPI_Pixel_Write(qspi, 0x2C);

	/* Clear the stall flag now so the completion handler can use it to tell
	   that the PIO has actually finished shifting. Write-one-to-clear. */
	qspi.pio->fdebug = Panel_TxStallMask();

	/* The vendor's own init sets this dreq to the receive direction, which is
	   wrong; every one of their transmit paths quietly overrides it on the way
	   past. Set it correctly here too rather than depending on that. */
	channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

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
static void Panel_FlushDoneIrq(void)
{
	uint32_t Spins = 0;

	if (!dma_channel_get_irq0_status(dma_tx))
		return;

	dma_channel_acknowledge_irq0(dma_tx);

	/* A finished DMA means the last byte reached the PIO FIFO, not that it has
	   been clocked out of it. Four words of FIFO and one in the output shift
	   register can still be pending, so raising chip select here - which is
	   what the vendor's handler does - cuts the final pixels off every flush.
	   Small enough to miss in a demo that redraws the whole screen; not small
	   enough in a gauge, where the flushed rectangle IS the gauge.

	   TXSTALL is the exact condition wanted: the state machine sets it when an
	   autopull finds the FIFO empty, which can only happen once the last
	   nibble has been shifted and clocked. Its side-set leaves the clock low
	   as it stalls, so nothing is left half-sent. */
	while ((qspi.pio->fdebug & Panel_TxStallMask()) == 0u)
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

	QSPI_Deselect(qspi);
	Flushes++;

	lv_disp_flush_ready(&DispDrv);
}


/***************************************************************************************/
/* Read the touch controller.
 *
 * POLLED, NOT INTERRUPT DRIVEN, AND THAT IS DELIBERATE.
 *
 * The vendor example hangs this off the touch interrupt: the handler reads one
 * point and latches PRESSED, and the read callback clears it to RELEASED on
 * its next call. That gives LVGL a press and a release around a single
 * coordinate - a tap - no matter what the finger actually did. A swipe needs
 * the press to stay asserted while the finger travels, or LVGL never
 * accumulates the distance it needs to report a gesture, and swiping between
 * faces is the reason there is a touch panel here at all (PLAN.md 4.3).
 *
 * So this asks the controller instead, and lets its own point count decide
 * when the finger has gone. One ten-byte register read at 400 kHz is a few
 * hundred microseconds every 20 ms, on the core that draws - not the core that
 * decodes CAN.
 */
static void Panel_TouchRead(lv_indev_drv_t *Drv, lv_indev_data_t *Data)
{
	(void)Drv;

	/* False here is NOT an error, and it was briefly counted as one: their
	   read returns false when byte 6 of the report is not 0xAB, and that
	   marker means "there is a report to read", not "the transfer worked".
	   With no finger on the glass there is nothing to report, so an idle
	   panel returns false on every poll. Whether the controller is there at
	   all is settled once, at init, by Panel_TouchProbe(). */
	if (!CST9217_Read_Data())
	{
		Data->state = LV_INDEV_STATE_RELEASED;
	}
	else if (CST9217.points > 0)
	{
		/* Their driver already maps the raw reading to display coordinates,
		   including the 180 degree flip, but 466 - 0 is 466 and the last valid
		   pixel is 465 - so clamp rather than hand LVGL a point one off the
		   edge. */
		uint16_t X = CST9217.data[0].x;
		uint16_t Y = CST9217.data[0].y;

		if (X >= PANEL_WIDTH)
			X = PANEL_WIDTH - 1u;
		if (Y >= PANEL_HEIGHT)
			Y = PANEL_HEIGHT - 1u;

		TouchX = (lv_coord_t)X;
		TouchY = (lv_coord_t)Y;
		TouchPresses++;
		Data->state = LV_INDEV_STATE_PRESSED;
	}
	else
	{
		Data->state = LV_INDEV_STATE_RELEASED;
	}

	/* The last known position travels with a release too. LVGL reads the point
	   on the releasing call to work out where the gesture ended; reporting
	   (0,0) there would look like a sudden drag to the corner. */
	Data->point.x = TouchX;
	Data->point.y = TouchY;
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
 * EVERY TRANSFER HERE IS BOUNDED, AND THAT IS NOT DECORATION.
 *
 * i2c_write_blocking() with nostop set returns an error on a NAK but leaves
 * the bus without a STOP, and the next transfer on it can then block forever.
 * That is not a hypothetical: an earlier version of this probe did exactly
 * that, and the LVGL input callback's read - which runs before the first
 * status line is printed - hung core 1 with no output at all. A touch
 * controller that does not answer must cost this function a few milliseconds,
 * not the display.
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
bool Panel_Init(void)
{
	bool Acked;

	/* ---- transport ---------------------------------------------------- */

	/* Their DEV_Module_Init() is deliberately not called. It would do four
	   useful things and two harmful ones: it calls stdio_init_all() a second
	   time, and it changes the system clock - which has to happen on core 0
	   before can2040 starts, not here. So the useful parts are spelled out. */
	QSPI_GPIO_Init(qspi);
	QSPI_PIO_Init(qspi);

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

	dma_tx = (uint)dma_claim_unused_channel(true);
	c = dma_channel_get_default_config(dma_tx);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, true);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, pio_get_dreq(qspi.pio, qspi.sm, true));

	/* ---- panel -------------------------------------------------------- */

	AMOLED_1IN75_Init();
	AMOLED_1IN75_Clear(BLACK);
	AMOLED_1IN75_SetBrightness(PANEL_DEFAULT_BRIGHTNESS);

	/* ---- touch -------------------------------------------------------- */

	i2c_init(I2C_PORT, 400 * 1000);
	gpio_set_function(DEV_SDA_PIN, GPIO_FUNC_I2C);
	gpio_set_function(DEV_SCL_PIN, GPIO_FUNC_I2C);
	gpio_pull_up(DEV_SDA_PIN);
	gpio_pull_up(DEV_SCL_PIN);

	gpio_init(TOUCH_RST_PIN);
	gpio_set_dir(TOUCH_RST_PIN, GPIO_OUT);

	/* CST9217_Init() would read the configuration and throw the result away.
	   The interrupt pin is left untouched - see Panel_TouchRead(). */
	CST9217_Reset();
	sleep_ms(30);
	Acked = Panel_TouchProbe(&TouchChipType);
	TouchPresent = Acked && (TouchChipType == CST9217_CHIP_ID);

	/* ---- LVGL --------------------------------------------------------- */

	lv_init();

	lv_disp_draw_buf_init(&DrawBufDesc, DrawBuf0, DrawBuf1, PANEL_BUF_PIXELS);

	lv_disp_drv_init(&DispDrv);
	DispDrv.draw_buf = &DrawBufDesc;
	DispDrv.flush_cb = Panel_Flush;
	DispDrv.hor_res = PANEL_WIDTH;
	DispDrv.ver_res = PANEL_HEIGHT;
	lv_disp_drv_register(&DispDrv);

	lv_indev_drv_init(&TouchDrv);
	TouchDrv.type = LV_INDEV_TYPE_POINTER;
	TouchDrv.read_cb = Panel_TouchRead;
	lv_indev_drv_register(&TouchDrv);

	/* Start centred, so a release before any press cannot read as a gesture
	   from the corner. */
	TouchX = PANEL_WIDTH / 2;
	TouchY = PANEL_HEIGHT / 2;

	/* ---- flush completion --------------------------------------------- */

	/* Installed from core 1, which is where it must run: the vector table is
	   per core, and lv_disp_flush_ready() has to be called on the core that
	   owns LVGL. Core 0 leaves DMA_IRQ_0 alone. */
	dma_channel_set_irq0_enabled(dma_tx, true);
	irq_set_exclusive_handler(DMA_IRQ_0, Panel_FlushDoneIrq);
	irq_set_enabled(DMA_IRQ_0, true);

	printf("panel: %dx%d, %u-line draw buffers (%lu bytes each)\n",
	       PANEL_WIDTH, PANEL_HEIGHT, (unsigned)PANEL_BUF_LINES,
	       (unsigned long)sizeof(DrawBuf0));
	if (TouchPresent)
		printf("touch: CST9217 on the bus\n");
	else if (Acked)
		printf("touch: something answered at 0x%02X but reports chip type "
		       "0x%04X, expected 0x%04X\n",
		       CST9217_I2C_ADDR, TouchChipType, CST9217_CHIP_ID);
	else
		printf("touch: no answer at 0x%02X - swiping unavailable\n",
		       CST9217_I2C_ADDR);

	return TouchPresent;
}


/***************************************************************************************/
uint32_t Panel_Service(void)
{
	return lv_timer_handler();
}


/***************************************************************************************/
void Panel_SetBrightness(uint8_t Percent)
{
	AMOLED_1IN75_SetBrightness(Percent);
}


/***************************************************************************************/
uint32_t Panel_Flushes(void)		{ return Flushes; }
uint32_t Panel_FlushTimeouts(void)	{ return FlushTimeouts; }
bool Panel_TouchPresent(void)		{ return TouchPresent; }
uint16_t Panel_TouchChipType(void)	{ return TouchChipType; }
uint32_t Panel_TouchPresses(void)	{ return TouchPresses; }

void Panel_TouchLast(uint16_t *X, uint16_t *Y)
{
	*X = (uint16_t)TouchX;
	*Y = (uint16_t)TouchY;
}
