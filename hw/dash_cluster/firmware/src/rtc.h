/*
 * rtc.h
 *
 * The board's PCF85063 real-time clock, on the I2C bus the touch controller
 * and the accelerometer share - so core 1 only, and every transfer bounded.
 *
 * IT IS NOT BACKED UP ON THIS BOARD. The PCF85063 has one supply pin, no
 * battery input, and the board's MX1.25 lithium connector is unpopulated - so
 * the rail dies with the ignition and the clock loses time every time the car
 * is switched off. The part says so itself: its seconds register carries an
 * oscillator-stop flag, set whenever the supply has been away, and Rtc_Read()
 * reports the time as untrustworthy while it stands. A node with no
 * trustworthy time shows none, rather than a plausible wrong one, and waits to
 * be told - from the console, or over CAN by a node that knows.
 *
 * So the RTC earns its place even unbacked: it keeps time across a page
 * change, a reflash and a watchdog reset, and it is the thing the CAN time
 * message writes into.
 */
#ifndef RTC_H_
#define RTC_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
	uint8_t Hours;			/* 0..23 */
	uint8_t Minutes;		/* 0..59 */
	uint8_t Seconds;		/* 0..59 */
} RtcTime_t;

/* Find the part and put it in a known state: 24-hour counting, its own
   oscillator running, no alarms - PLAN.md leaves GPIO27 free on the promise
   that the alarm interrupt stays off. False if nothing answered. */
extern bool Rtc_Init(void);

/* The time, and whether it can be trusted: false while the part's
   oscillator-stop flag stands, which is any time since it lost power. */
extern bool Rtc_Read(RtcTime_t *Out);

/* Set it, and clear the stop flag - the time is trustworthy from here. */
extern bool Rtc_Write(const RtcTime_t *Time);

/* For the console: whether the part is there at all, and failed transfers. */
extern bool Rtc_Present(void);
extern uint32_t Rtc_Errors(void);

#endif /* RTC_H_ */
