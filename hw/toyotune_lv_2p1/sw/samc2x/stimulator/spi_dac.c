/*
 * spi_dac.c
 *
 * Created: 06/06/2020 17:07:06
 *  Author: jonso
 */ 

#include "sercom.h"
#include "pio.h"

#include "spi_dac.h"

void SPIDAC_Init(void)
{
	SercomSpi *Spi = &SERCOM5->SPI;
	
	/* Enable USART clock in PMC */
	SERCOM_EnableClock(5);

	PIO_SetPeripheral(PIN_PB00, PIO_PERIPHERAL_D);
	PIO_EnablePeripheral(PIN_PB00);
	PIO_SetPeripheral(PIN_PB01, PIO_PERIPHERAL_D);
	PIO_EnablePeripheral(PIN_PB01);
	PIO_SetPeripheral(PIN_PB02, PIO_PERIPHERAL_D);
	PIO_EnablePeripheral(PIN_PB02);

	//PB00 - PAD 2 - MOSI
	//PB01 - PAD 3 - CLK
	
	/* Initialise SPI */
	SERCOM_SpiInit(5, SERCOM_SPI_CTRLA_FORM(0) | SERCOM_SPI_CTRLA_DIPO(0) | SERCOM_SPI_CTRLA_DOPO(1) | SERCOM_SPI_CTRLA_MODE(3),
				   SERCOM_SPI_CTRLB_CHSIZE(0), 4000);
				   
	/* Enable SPI */
	SERCOM_SpiEnable(Spi);
}


void SPIDAC_Write(uint8_t CsPio, uint16_t Data)
{
	SercomSpi *Spi = &SERCOM5->SPI;
	PIO_Clear(CsPio);
	Spi->DATA.reg = Data >> 8;	
	while (!SERCOM_SpiTxReady(Spi));
	Spi->DATA.reg = Data & 0xFF;	
	while (!SERCOM_SpiTxComplete(Spi));
	PIO_Set(CsPio);
}

void SPIDAC_SetOutputVoltage(uint8_t CsPio, uint16_t Mv)
{
	uint16_t Data = (4096UL * Mv / 5000);
	if (Data > 4095)
		Data = 4095;
	SPIDAC_Write(CsPio, (1 << 13) + (1 << 12) + Data);
}