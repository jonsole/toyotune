/*
 * can.h
 *
 * Created: 08/06/2023 20:52:02
 *  Author: jonso
 */ 


#ifndef CAN_H_
#define CAN_H_

#include <stdint.h>

void CAN_Init(void);
void CAN_Tx(uint32_t Id, const void *Data, uint32_t DataSize);
void CAN_TxStandard(uint16_t Id, const void *Data, uint32_t DataSize);

#endif /* CAN_H_ */