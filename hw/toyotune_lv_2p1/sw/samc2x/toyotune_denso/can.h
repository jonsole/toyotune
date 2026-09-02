/*
 * can.h
 *
 * Created: 08/06/2023 20:52:02
 *  Author: jonso
 */ 


#ifndef CAN_H_
#define CAN_H_

#include <stdint.h>
#include <stdbool.h>

/* Count of frames dropped because the Tx queue stayed full for the whole
   bounded wait. Zero on a healthy bus; a rising count means the bus is
   congested, or nothing is acknowledging and the controller is heading for
   bus-off. Readable over SWD, which is how you tell "no telemetry because
   the bus is dead" from "no telemetry because the firmware is stuck". */
extern uint32_t CAN_TxDropped;

/* Count of bus-off recoveries. Non-zero means the controller has taken itself
   off the bus at least once - normally because nothing was acknowledging. */
extern uint32_t CAN_BusOffRecoveries;

/* Called from CAN0_Handler in INTERRUPT context for each frame accepted by
   the receive filter.  Keep it short and do not block; hand real work to a
   task. Data points into the receive FIFO element and is only valid for the
   duration of the call. */
typedef void (*CAN_RxHandler_t)(uint16_t Id, const uint8_t *Data, uint8_t Length);
void CAN_RxSetHandler(CAN_RxHandler_t Handler);

void CAN_Init(void);

/* Call periodically. Returns the controller to the bus if it has gone
   bus-off, which it will not do by itself. */
void CAN_Poll(void);

/* Queue a frame. Returns false if no queue slot came free in time and the
   frame was dropped. */
bool CAN_Tx(uint32_t Id, const void *Data, uint32_t DataSize);
bool CAN_TxStandard(uint16_t Id, const void *Data, uint32_t DataSize);

#endif /* CAN_H_ */