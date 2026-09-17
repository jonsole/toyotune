/*
 * imu.c - the QMI8658 accelerometer. See imu.h.
 */

#include "imu.h"

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/i2c.h"

#include "DEV_Config.h"		/* I2C_PORT, shared with the touch controller */

/* The part answers at one of two addresses depending on how its SA0 pin is
   strapped; both are tried rather than one assumed. */
#define IMU_ADDR_A		(0x6Bu)
#define IMU_ADDR_B		(0x6Au)

#define IMU_REG_WHO_AM_I	(0x00u)
#define IMU_REG_REVISION	(0x01u)
#define IMU_REG_CTRL1		(0x02u)		/* serial interface */
#define IMU_REG_CTRL2		(0x03u)		/* accelerometer range and rate */
#define IMU_REG_CTRL5		(0x06u)		/* low-pass filters */
#define IMU_REG_CTRL7		(0x08u)		/* sensor enables */
#define IMU_REG_ACCEL_X_L	(0x35u)		/* X, Y, Z, low byte first */
#define IMU_REG_RESET		(0x60u)

#define IMU_WHO_AM_I_VALUE	(0x05u)
#define IMU_RESET_VALUE		(0xB0u)

/* CTRL1: register address auto-increment on, so the six data bytes come in
   one read; little-endian data; internal oscillator on. */
#define IMU_CTRL1_VALUE		(0x40u)

/* CTRL2: +-4 g, 125 Hz. Four rather than two because a kerb or a pothole
   easily passes 2 g and a clipped sample would read as a plausible lower one;
   125 Hz is twice the display's rate. */
#define IMU_CTRL2_VALUE		(0x16u)
#define IMU_LSB_PER_G		(8192)

/* CTRL5: the accelerometer's own low-pass filter on, at its widest setting -
   about an eighth of the output rate. The display smooths further; this only
   keeps engine vibration out of the samples. */
#define IMU_CTRL5_VALUE		(0x07u)

/* CTRL7: accelerometer on, gyro off. */
#define IMU_CTRL7_VALUE		(0x01u)

/* Bound on every transfer, as for touch. */
#define IMU_I2C_TIMEOUT_US	(5000u)

static bool		Present;
static uint8_t		Address;
static uint8_t		Revision;
static uint32_t		ReadErrors;


/***************************************************************************************/
static bool Imu_Write(uint8_t Addr, uint8_t Reg, uint8_t Value)
{
	uint8_t Buf[2];

	Buf[0] = Reg;
	Buf[1] = Value;
	return i2c_write_timeout_us(I2C_PORT, Addr, Buf, sizeof(Buf), false,
	                            IMU_I2C_TIMEOUT_US) == (int)sizeof(Buf);
}


/***************************************************************************************/
/* Register address, then a read with a repeated start. If the address write
   fails, a one-byte read afterwards is what puts a STOP back on the bus - the
   same recovery the touch code needs. */
static bool Imu_ReadReg(uint8_t Addr, uint8_t Reg, uint8_t *Out, uint32_t Len)
{
	if (i2c_write_timeout_us(I2C_PORT, Addr, &Reg, 1, true, IMU_I2C_TIMEOUT_US) != 1)
	{
		(void)i2c_read_timeout_us(I2C_PORT, Addr, Out, 1, false, IMU_I2C_TIMEOUT_US);
		return false;
	}

	return i2c_read_timeout_us(I2C_PORT, Addr, Out, Len, false, IMU_I2C_TIMEOUT_US)
	       == (int)Len;
}


/***************************************************************************************/
bool Imu_Init(void)
{
	static const uint8_t Candidates[] = { IMU_ADDR_A, IMU_ADDR_B };
	uint8_t Who = 0;
	size_t i;

	Present = false;

	for (i = 0; i < sizeof(Candidates); i++)
	{
		if (Imu_ReadReg(Candidates[i], IMU_REG_WHO_AM_I, &Who, 1)
		    && Who == IMU_WHO_AM_I_VALUE)
		{
			Address = Candidates[i];
			break;
		}
	}

	if (i == sizeof(Candidates))
	{
		printf("imu: no QMI8658 at 0x%02x or 0x%02x (last id 0x%02x)\n",
		       IMU_ADDR_A, IMU_ADDR_B, Who);
		return false;
	}

	(void)Imu_Write(Address, IMU_REG_RESET, IMU_RESET_VALUE);
	sleep_ms(20);

	if (!Imu_ReadReg(Address, IMU_REG_REVISION, &Revision, 1)
	    || !Imu_Write(Address, IMU_REG_CTRL1, IMU_CTRL1_VALUE)
	    || !Imu_Write(Address, IMU_REG_CTRL2, IMU_CTRL2_VALUE)
	    || !Imu_Write(Address, IMU_REG_CTRL5, IMU_CTRL5_VALUE)
	    || !Imu_Write(Address, IMU_REG_CTRL7, IMU_CTRL7_VALUE))
	{
		printf("imu: QMI8658 at 0x%02x did not take its configuration\n", Address);
		return false;
	}

	/* Let the first samples through the filter. */
	sleep_ms(30);
	Present = true;
	printf("imu: QMI8658 at 0x%02x, revision 0x%02x\n", Address, Revision);
	return true;
}


/***************************************************************************************/
bool Imu_Read(ImuMilliG_t *Out)
{
	uint8_t D[6];
	int32_t Raw[3];
	int k;

	if (!Present)
		return false;

	if (!Imu_ReadReg(Address, IMU_REG_ACCEL_X_L, D, sizeof(D)))
	{
		ReadErrors++;
		return false;
	}

	for (k = 0; k < 3; k++)
		Raw[k] = (int32_t)(int16_t)(uint16_t)((uint16_t)D[2 * k]
		                                      | (uint16_t)((uint16_t)D[(2 * k) + 1] << 8));

	Out->X = (Raw[0] * 1000) / IMU_LSB_PER_G;
	Out->Y = (Raw[1] * 1000) / IMU_LSB_PER_G;
	Out->Z = (Raw[2] * 1000) / IMU_LSB_PER_G;
	return true;
}


/***************************************************************************************/
bool Imu_Present(void)		{ return Present; }
uint8_t Imu_Address(void)	{ return Address; }
uint8_t Imu_Revision(void)	{ return Revision; }
uint32_t Imu_ReadErrors(void)	{ return ReadErrors; }
