#ifndef _TOYOTUNE_AVR_H_
#define _TOYOTUNE_AVR_H_

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <avr/interrupt.h>
#include <avr/sleep.h>
#include <util/atomic.h>
#include <avr/eeprom.h>

#include <string.h>
#include <stdbool.h>

#include "config.h"
#include "hal.h"
#include "hal_memory.h"
#include "hal_sram.h"
#include "hal_uart.h"

typedef struct
{
	uint32_t Major;
	uint16_t Minor;
} Toy_SoftwareID_t;

extern void Toy_GetSoftwareID(Toy_SoftwareID_t *SoftwareID );

#endif

