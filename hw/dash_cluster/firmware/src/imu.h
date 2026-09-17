/*
 * imu.h
 *
 * The board's QMI8658 6-axis IMU, accelerometer only, for the g-force page.
 *
 * On the I2C bus the touch controller shares, so core 1 only - the same core
 * that services touch - and every transfer is bounded, for the same reason as
 * there: a device that stops answering must cost a frame, not the display.
 *
 * THE REGISTER SETTINGS ARE CHECKED, NOT TRUSTED. They were written from the
 * part's documented register map rather than from a driver known to work on
 * this board, so Imu_Init() insists the part identifies itself, and the
 * console reports the total acceleration: a board at rest must read 1.00 g,
 * and a wrong range or scale setting cannot.
 */
#ifndef IMU_H_
#define IMU_H_

#include <stdbool.h>
#include <stdint.h>

/* Acceleration in thousandths of a g, in the SENSOR's axes. Which of those is
   the car's lateral and longitudinal depends on how the node is mounted - see
   the g page's calibration. */
typedef struct
{
	int32_t X;
	int32_t Y;
	int32_t Z;
} ImuMilliG_t;

/* Find and configure the part. Core 1, after Panel_Init() has brought the I2C
   bus up. False if nothing answered as a QMI8658 - the g page then shows no
   data rather than a wrong reading. */
extern bool Imu_Init(void);

/* The latest sample, if one could be read. */
extern bool Imu_Read(ImuMilliG_t *Out);

/* For the console: whether it was found, at which address, its revision, and
   failed reads since boot. */
extern bool Imu_Present(void);
extern uint8_t Imu_Address(void);
extern uint8_t Imu_Revision(void);
extern uint32_t Imu_ReadErrors(void);

#endif /* IMU_H_ */
