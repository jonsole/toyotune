/*
 * can_telemetry.h
 *
 * Periodic CAN telemetry built from the inter-CPU DMA capture.
 */


#ifndef CAN_TELEMETRY_H_
#define CAN_TELEMETRY_H_

#include <stdint.h>

/* Wire-protocol version, broadcast in the INFO frame.
 *
 * Bump this whenever a frame's layout, a signal's position, or a signal's
 * units change - anything that would make an older decoder wrong rather than
 * merely incomplete. Adding a new frame at a new identifier does not need a
 * bump, since an old decoder simply ignores it.
 *
 * toyotune.dbc must carry the same number. Nothing enforces that, which is
 * exactly why the version is on the bus: a consumer that finds a version it
 * does not recognise can say so instead of drawing gauges from bytes that
 * have moved.
 *
 * 1 - engineering units. Signals converted on the board rather than left as
 *     raw counts; MEDIUM1 regrouped to four 16-bit values; knock retards
 *     moved to the new MEDIUM3; injector duty derived into MEDIUM2; INFO
 *     added. Not compatible with any earlier capture.
 */
#define TOYOTUNE_TELEMETRY_PROTOCOL_VERSION	(1)

/* Starts the telemetry task. Call after CAN_Init() and the SDL instances. */
extern void CanTelemetry_Init(void);

/* Call from SysTick, once per millisecond. Cheap - it counts, and only signals
   the task when a tick boundary is reached. */
extern void CanTelemetry_TimerTick(void);

#endif /* CAN_TELEMETRY_H_ */
