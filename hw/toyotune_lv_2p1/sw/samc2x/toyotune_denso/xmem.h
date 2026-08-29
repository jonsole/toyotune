/*
 * xmem.h
 *
 * Created: 10/11/2020 14:06:24
 *  Author: jonso
 */ 


#ifndef XMEM_H_
#define XMEM_H_

#include <sam.h>
#include <stdbool.h>

extern void XMEM_Init(void);
extern void XMEM_BlockWrite(uint16_t DestAddr, const uint8_t *SrcPtr, uint16_t Size);
extern void XMEM_BlockRead(uint8_t *DestPtr, uint16_t SrcAddr, uint16_t Size);
extern void XMEM_WriteEnable(bool Enabled);

extern void XMEM_AddressTest(void);
extern void XMEM_PinTest(uint8_t Pin);


#endif /* XMEM_H_ */