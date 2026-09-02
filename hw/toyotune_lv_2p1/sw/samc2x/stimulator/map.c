/*
 * map.c
 *
 * Created: 10/06/2020 21:27:57
 *  Author: jonso
 */ 

#include <sam.h>

#include "pio.h"

#include "map.h"
#include "spi_dac.h"


// from http://gtfour.supras.org.nz/mapsensor.htm
// ST205:    V = 0.1025*psi + 2.3293  up to a maximum of 5V




#define PIO_PIM		(PIN_PA27)

#define MAP_MIN_MV		(50)
#define MAP_MAX_MV		(4950)


void MAP_Init(void)
{
	PIO_Set(PIO_PIM);
	PIO_EnableOutput(PIO_PIM);
	MAP_Set(500);
}


void MAP_Set(uint16_t Millibar)
{
	// 1 bar = 14.5038 psi
	// 1000 millibar = 1 bar
	// 1000 millibar = 14.5038 psi
	// 1 psi = 1000 / 14.5038 = 68.9476 millibar
	// 1 millibar = 1 / 68.9476 psi
	// v = 0.1025 * psi + 2.3293
	//mv =  102.5 * psi + 2329.3
	//mv =  (102.5 * mb / 68.9476) + 2329.3
	//mv =  (1025000 * mb / 689476) + 2329
	
	const uint16_t Voltage = 2329 + (1025000 * (Millibar - 1000) / 689476);
	SPIDAC_SetOutputVoltage(PIO_PIM, Voltage);

}

