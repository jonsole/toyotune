/*
 * can.h
 *
 * CAN driver for the stimulator. Note the transmit path in main() is a bus
 * test, not telemetry - see the comment there before putting this board on a
 * bus shared with the Toyotune board.
 */


#ifndef CAN_H_
#define CAN_H_

void CAN_Init(void);
void CAN_Tx(uint32_t Id, void *Data, uint32_t DataSize);


#endif /* CAN_H_ */
