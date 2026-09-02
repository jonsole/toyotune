/*
 * spi_dac.h
 *
 * External SPI DAC driving the analogue sensor outputs (MAP, throttle
 * position). CsPio selects which DAC is addressed.
 */


#ifndef SPI_DAC_H_
#define SPI_DAC_H_

void SPIDAC_Init(void);
void SPIDAC_Write(uint8_t CsPio, uint16_t Data);
void SPIDAC_SetOutputVoltage(uint8_t CsPio, uint16_t Mv);


#endif /* SPI_DAC_H_ */
