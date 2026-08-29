/*
 * pio.h
 *
 * Created: 11/07/2016 21:05:39
 *  Author: Jon
 */ 

#include <sam.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef PIO_H_
#define PIO_H_

enum
{
	PIO_PERIPHERAL_A = 0,
	PIO_PERIPHERAL_B,
	PIO_PERIPHERAL_C,
	PIO_PERIPHERAL_D,
	PIO_PERIPHERAL_E,
	PIO_PERIPHERAL_F,
	PIO_PERIPHERAL_G,
	PIO_PERIPHERAL_H,
	PIO_PERIPHERAL_I,
};

static __inline uint8_t PIO_GetPioGroup(uint8_t Pio)
{
	return Pio >> 5;
}

static __inline void PIO_EnablePeripheral(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	PORT->Group[Group].PINCFG[Pio % 32].bit.PMUXEN = 1;
}

static __inline void PIO_SetPeripheral(uint8_t Pio, uint8_t Peripheral)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	const uint8_t Pmux = (Pio % 32) / 2;
	
	/* Check if odd or even PIO */
	if (Pio % 2)
		PORT->Group[Group].PMUX[Pmux].bit.PMUXO = Peripheral;
	else
		PORT->Group[Group].PMUX[Pmux].bit.PMUXE = Peripheral;
}

static __inline void PIO_EnableInput(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	PORT->Group[Group].PINCFG[Pio % 32].bit.INEN = 1;
	PORT->Group[Group].CTRL.bit.SAMPLING = 1;
}

static __inline void PIO_EnableOutput(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	PORT->Group[Group].DIRSET.reg = 1UL << (Pio % 32);
}

static __inline void PIO_DisableOutput(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	PORT->Group[Group].DIRCLR.reg = 1UL << (Pio % 32);
}

static __inline void PIO_Set(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	PORT->Group[Group].OUTSET.reg = 1UL << (Pio % 32);
}

static __inline void PIO_Clear(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	PORT->Group[Group].OUTCLR.reg = 1UL << (Pio % 32);
}

static __inline void PIO_Toggle(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	PORT->Group[Group].OUTTGL.reg = 1UL << (Pio % 32);
}

static __inline void PIO_SetEventAction(uint8_t Pio, uint8_t Event, uint8_t Action)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);

	PORT->Group[Group].EVCTRL.bit.EVACT0 = Action;
	PORT->Group[Group].EVCTRL.bit.PID0 = Pio % 32;
	PORT->Group[Group].EVCTRL.bit.PORTEI0 = 1;
}

static __inline bool PIO_IsHigh(uint8_t Pio)
{
	const uint8_t Group = PIO_GetPioGroup(Pio);
	return PORT->Group[Group].IN.reg & (1UL << (Pio % 32));
}

static __inline bool PIO_IsLow(uint8_t Pio)
{
	return !PIO_IsHigh(Pio);
}

#endif /* PIO_H_ */