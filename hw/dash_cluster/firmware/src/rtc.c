/*
 * rtc.c - the PCF85063 clock. See rtc.h.
 */

#include "rtc.h"

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "DEV_Config.h"		/* I2C_PORT, shared with touch and the IMU */

#define RTC_ADDR		(0x51u)		/* fixed on the PCF85063 */

#define RTC_REG_CONTROL1	(0x00u)
#define RTC_REG_CONTROL2	(0x01u)
#define RTC_REG_SECONDS		(0x04u)		/* then minutes, hours, days... */

/* Control_1: 24-hour mode, internal oscillator on, no test modes, and the
   capacitor selection left at the reset default - the crystal's load is a
   board matter and 0 is the part's own default. */
#define RTC_CONTROL1_VALUE	(0x00u)

/* Control_2: no alarm interrupt, no minute/half-minute interrupt. GPIO27 is
   free only while this is true - see PLAN.md section 4.1. */
#define RTC_CONTROL2_VALUE	(0x00u)

/* The seconds register's top bit: set whenever the oscillator has stopped,
   which includes every time the supply has been away. While it stands, the
   time held is not a time. */
#define RTC_SECONDS_STOPPED	(0x80u)

#define RTC_I2C_TIMEOUT_US	(5000u)

static bool		Present;
static uint32_t		Errors;


/***************************************************************************************/
static uint8_t Rtc_FromBcd(uint8_t V)
{
	return (uint8_t)(((V >> 4) * 10u) + (V & 0x0Fu));
}

static uint8_t Rtc_ToBcd(uint8_t V)
{
	return (uint8_t)(((V / 10u) << 4) | (V % 10u));
}


/***************************************************************************************/
static bool Rtc_WriteRegs(uint8_t Reg, const uint8_t *Data, uint32_t Len)
{
	uint8_t Buf[8];
	uint32_t i;

	if (Len + 1u > sizeof(Buf))
		return false;

	Buf[0] = Reg;
	for (i = 0; i < Len; i++)
		Buf[i + 1u] = Data[i];

	if (i2c_write_timeout_us(I2C_PORT, RTC_ADDR, Buf, Len + 1u, false,
	                         RTC_I2C_TIMEOUT_US) != (int)(Len + 1u))
	{
		Errors++;
		return false;
	}
	return true;
}


/***************************************************************************************/
/* Register address, then a read with a repeated start; a one-byte read after a
   failed address write is what puts a STOP back on the bus - the same recovery
   the touch code needs. */
static bool Rtc_ReadRegs(uint8_t Reg, uint8_t *Data, uint32_t Len)
{
	if (i2c_write_timeout_us(I2C_PORT, RTC_ADDR, &Reg, 1, true, RTC_I2C_TIMEOUT_US) != 1)
	{
		(void)i2c_read_timeout_us(I2C_PORT, RTC_ADDR, Data, 1, false, RTC_I2C_TIMEOUT_US);
		Errors++;
		return false;
	}

	if (i2c_read_timeout_us(I2C_PORT, RTC_ADDR, Data, Len, false, RTC_I2C_TIMEOUT_US)
	    != (int)Len)
	{
		Errors++;
		return false;
	}
	return true;
}


/***************************************************************************************/
bool Rtc_Init(void)
{
	uint8_t Control[2] = { RTC_CONTROL1_VALUE, RTC_CONTROL2_VALUE };
	uint8_t Seconds = 0;

	Present = false;

	/* The part has no identity register, so presence is simply whether it
	   answers - and whether what it answers with is a possible time. */
	if (!Rtc_ReadRegs(RTC_REG_SECONDS, &Seconds, 1))
	{
		printf("rtc: no PCF85063 at 0x%02x\n", RTC_ADDR);
		return false;
	}

	if (Rtc_FromBcd((uint8_t)(Seconds & 0x7Fu)) > 59u)
	{
		printf("rtc: 0x%02x answered with 0x%02x in its seconds register, "
		       "which is not a time\n", RTC_ADDR, Seconds);
		return false;
	}

	if (!Rtc_WriteRegs(RTC_REG_CONTROL1, Control, sizeof(Control)))
	{
		printf("rtc: PCF85063 did not take its configuration\n");
		return false;
	}

	Present = true;
	printf("rtc: PCF85063 at 0x%02x, %s\n", RTC_ADDR,
	       ((Seconds & RTC_SECONDS_STOPPED) != 0u) ? "time LOST since power-up"
	                                               : "time held");
	return true;
}


/***************************************************************************************/
bool Rtc_Read(RtcTime_t *Out)
{
	uint8_t D[3];

	if (!Present || !Rtc_ReadRegs(RTC_REG_SECONDS, D, sizeof(D)))
		return false;

	/* Its own stop flag: the time behind it is whatever it was counting from
	   when the supply went, so it is not a reading. */
	if ((D[0] & RTC_SECONDS_STOPPED) != 0u)
		return false;

	Out->Seconds = Rtc_FromBcd((uint8_t)(D[0] & 0x7Fu));
	Out->Minutes = Rtc_FromBcd((uint8_t)(D[1] & 0x7Fu));
	Out->Hours = Rtc_FromBcd((uint8_t)(D[2] & 0x3Fu));

	/* A register full of nonsense - a bus fault, or a part counting in
	   12-hour mode after a reset nobody saw - must not become a displayed
	   time. */
	return Out->Seconds <= 59u && Out->Minutes <= 59u && Out->Hours <= 23u;
}


/***************************************************************************************/
bool Rtc_Write(const RtcTime_t *Time)
{
	uint8_t D[3];

	if (!Present || Time->Hours > 23u || Time->Minutes > 59u || Time->Seconds > 59u)
		return false;

	/* Writing the seconds register with the stop bit clear is what marks the
	   time trustworthy again. */
	D[0] = Rtc_ToBcd(Time->Seconds);
	D[1] = Rtc_ToBcd(Time->Minutes);
	D[2] = Rtc_ToBcd(Time->Hours);
	return Rtc_WriteRegs(RTC_REG_SECONDS, D, sizeof(D));
}


/***************************************************************************************/
bool Rtc_Present(void)		{ return Present; }
uint32_t Rtc_Errors(void)	{ return Errors; }
