/*
 * throttle.c
 *
 * Created: 10/06/2020 20:53:25
 *  Author: jonso
 */ 

#include "pio.h"

#include "throttle.h"
#include "spi_dac.h"

#define PIO_VTA		(PIN_PB05)
#define PIO_IDLE	(PIN_PB06)  // TODO

#define THROTTLE_MIN_MV		(1000)
#define THROTTLE_MAX_MV		(4000)

#define THROTTLE_IDLE		(10)


void Throttle_Init(void)
{
	PIO_Set(PIO_VTA);
	PIO_EnableOutput(PIO_VTA);
	SPIDAC_SetOutputVoltage(PIO_VTA, THROTTLE_MIN_MV);	
}


void Throttle_Set(uint8_t Value)
{
	const uint16_t Voltage = ((THROTTLE_MAX_MV - THROTTLE_MIN_MV) * Value / 100) + THROTTLE_MIN_MV;
	SPIDAC_SetOutputVoltage(PIO_VTA, Voltage);
	
	if (Value <= THROTTLE_IDLE)
		PIO_Clear(PIO_IDLE);
	else
		PIO_Set(PIO_IDLE);
}
