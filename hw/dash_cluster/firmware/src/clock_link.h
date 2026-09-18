/*
 * clock_link.h
 *
 * The time, announced on the CAN bus.
 *
 * The dash nodes have no trustworthy clock of their own: the PCF85063 is not
 * backed up on this board, so every ignition cycle starts with the time lost
 * (see rtc.h). Something else on the bus announces it, and a node takes what
 * it hears - which also keeps three gauges showing the same time without any
 * of them being the master.
 *
 * A NODE NEVER ANNOUNCES. It only listens. Three nodes arguing about the time
 * would be a worse failure than a blank clock, and the announcer is a device
 * with a reason to know - a GPS, a body module, a bench PC.
 *
 * THE FRAME, which the announcer must match. Standard identifier
 * CLOCK_LINK_TIME_ID, sent about once a second; four bytes are enough, eight
 * if the date is carried:
 *
 *   byte 0   flags: bit 0 set when the sender's own time is trustworthy.
 *                   A frame with it clear is ignored - an announcer that has
 *                   not got the time yet must say so rather than send zeros.
 *   byte 1   hours, 0..23, binary (not BCD)
 *   byte 2   minutes, 0..59
 *   byte 3   seconds, 0..59
 *   byte 4   day of month, 1..31     ) carried for later; a node's clock face
 *   byte 5   month, 1..12            ) shows only the time, and these are
 *   byte 6   year, high byte         ) accepted and ignored
 *   byte 7   year, low byte          )
 *
 * Binary rather than BCD, and hours before seconds, so the frame reads the way
 * a clock does in a log. Anything that does not parse is counted and dropped:
 * a wrong announcer must not be able to put a plausible wrong time on a dash.
 */
#ifndef CLOCK_LINK_H_
#define CLOCK_LINK_H_

#include <stdbool.h>
#include <stdint.h>

#include "rtc.h"

/* Clear of the ECU telemetry blocks at 0x400 and 0x420 and of the node
   heartbeats, which run 0x440 + node id for four identities. */
#define CLOCK_LINK_TIME_ID	(0x450u)

#define CLOCK_LINK_FLAG_VALID	(0x01u)

extern void ClockLink_Init(void);

/* Offer a received frame. True if it was the time and it parsed. Core 0, from
   the CAN callback's poll. */
extern bool ClockLink_Handle(uint16_t Id, const uint8_t *Data, uint8_t Length);

/* Take an announced time not yet acted on, if there is one. Core 1, which owns
   the I2C bus the clock is on. */
extern bool ClockLink_Take(RtcTime_t *Out);

/* For the console: frames accepted, and frames that claimed to be the time
   and were not. */
extern uint32_t ClockLink_Accepted(void);
extern uint32_t ClockLink_Rejected(void);

#endif /* CLOCK_LINK_H_ */
