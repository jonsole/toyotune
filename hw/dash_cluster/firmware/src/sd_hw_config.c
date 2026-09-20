/*
 * sd_hw_config.c
 *
 * Where the microSD socket is, for carlk3's FatFs library - which asks the
 * application for this rather than carrying a board definition.
 *
 * Taken from Waveshare's own FatFs example for this module
 * (examples/C/03_FatFs/example/config/hw_config.c), not from the pinout
 * image, which PLAN.md records as having at least two errors.
 *
 * SPI, NOT SDIO. The library's SDIO driver is faster and uses a PIO block -
 * and the only block left is PIO1, which is can2040's. Losing CAN to gain
 * card speed would be a poor trade for a log; SPI needs no PIO at all.
 *
 * The card detect pin is enabled here even though Waveshare leave it off:
 * with it, a card that is not in the socket is known immediately instead of
 * after a mount attempt times out, and pulling the card out mid-drive stops
 * the log rather than filling the console with write errors.
 */

#include "hw_config.h"

/* 25 MHz is within every card's SPI mode rating, and the log needs a few KB a
   second - the speed is irrelevant here, so take the reliable number. */
#define SD_BAUD_RATE	(25 * 1000 * 1000)

static spi_t Spi = {
	.hw_inst = spi0,		/* SCK 18, MOSI 19, MISO 20 - nothing else uses spi0 */
	.sck_gpio = 18,
	.mosi_gpio = 19,
	.miso_gpio = 20,
	.baud_rate = SD_BAUD_RATE,
	.sck_gpio_drive_strength = GPIO_DRIVE_STRENGTH_12MA,
	.mosi_gpio_drive_strength = GPIO_DRIVE_STRENGTH_4MA,
	.no_miso_gpio_pull_up = true,
};

static sd_spi_if_t SpiIf = {
	.spi = &Spi,
	.ss_gpio = 21,
};

static sd_card_t Card = {
	.type = SD_IF_SPI,
	.spi_if_p = &SpiIf,
	.use_card_detect = true,
	.card_detect_gpio = 24,		/* GPS_RST on the pinout image; card detect in their code */
	.card_detected_true = 0,	/* pulled up, taken low by a card */
	.card_detect_use_pull = true,
	.card_detect_pull_hi = true,
};


/***************************************************************************************/
size_t sd_get_num(void)
{
	return 1;
}


/***************************************************************************************/
sd_card_t *sd_get_by_num(size_t Num)
{
	return (Num == 0) ? &Card : NULL;
}
