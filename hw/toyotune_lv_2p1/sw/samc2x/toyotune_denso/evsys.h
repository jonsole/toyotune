/*
 * evsys.h
 *
 * Created: 13/02/2020 20:29:40
 *  Author: WinUser
 */ 


#ifndef EVSYS_H_
#define EVSYS_H_

#include <sam.h>

extern void EVSYS_Init(void);
extern void EVSYS_AsyncChannel(uint8_t Channel, uint8_t Generator, uint8_t User);


#endif /* EVSYS_H_ */