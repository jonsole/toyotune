/*
 * can_telemetry.h
 *
 * Periodic CAN telemetry built from the inter-CPU DMA capture.
 */


#ifndef CAN_TELEMETRY_H_
#define CAN_TELEMETRY_H_

#include <stdint.h>

/* Starts the telemetry task. Call after CAN_Init() and the SDL instances. */
extern void CanTelemetry_Init(void);

/* Call from SysTick, once per millisecond. Cheap - it counts, and only signals
   the task when a tick boundary is reached. */
extern void CanTelemetry_TimerTick(void);

#endif /* CAN_TELEMETRY_H_ */
