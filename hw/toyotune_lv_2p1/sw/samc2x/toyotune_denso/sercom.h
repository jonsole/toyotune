/*
 * sercom.h
 *
 * Created: 17/10/2016 21:24:31
 *  Author: Jon
 */ 


#ifndef SERCOM_H_
#define SERCOM_H_

#include <stdint.h>
#include <stdbool.h>
#include <sam.h>

static __inline uint8_t SERCOM_DmaTxTrigger(uint8_t Instance)
{
	return 0x03 + (Instance * 2);
}

static __inline uint8_t SERCOM_DmaRxTrigger(uint8_t Instance)
{
	return 0x02 + (Instance * 2);
}

static __inline uint8_t SERCOM_GetIrqNumber(uint8_t Instance)
{
	return 9 + Instance;
}

static __inline Sercom *SERCOM_GetSercom(uint8_t Instance)
{
	Sercom* InstanceTable[] =
	{
		SERCOM0, SERCOM1, SERCOM2, SERCOM3, SERCOM4, SERCOM5
	};
	return InstanceTable[Instance];
}

static __inline void SERCOM_UsartSyncWait(SercomUsart *Usart)
{
	while (Usart->SYNCBUSY.bit.ENABLE);
}

static __inline void SERCOM_UsartEnable(SercomUsart *Usart)
{
	Usart->CTRLA.reg |= SERCOM_USART_CTRLA_ENABLE;
	SERCOM_UsartSyncWait(Usart);
}

static __inline void SERCOM_UsartDisable(SercomUsart *Usart)
{
	Usart->CTRLA.reg &= ~SERCOM_USART_CTRLA_ENABLE;
	SERCOM_UsartSyncWait(Usart);
}

static __inline void SERCOM_UsartTxEnable(SercomUsart *Usart)
{
	Usart->CTRLB.bit.TXEN = 1;
}

static __inline void SERCOM_UsartTxDisable(SercomUsart *Usart)
{
	Usart->CTRLB.bit.TXEN = 0;
}

static __inline bool SERCOM_UsartTxReady(SercomUsart *Usart)
{
	return Usart->INTFLAG.bit.DRE;
}

/* DRE only means the DATA register is free; the byte may still be sitting in
   the shift register.  On this link the SAMC21 is the synchronous SLAVE - a
   frame leaves only when the ECU clocks it - so clearing TXEN on DRE alone can
   discard a byte that never went out.  TXC means "actually shifted out". */
static __inline bool SERCOM_UsartTxComplete(SercomUsart *Usart)
{
	return Usart->INTFLAG.bit.TXC;
}

static __inline void SERCOM_UsartTxClearComplete(SercomUsart *Usart)
{
	Usart->INTFLAG.reg = SERCOM_USART_INTFLAG_TXC;
}

static __inline void SERCOM_UsartSetRxPad(SercomUsart *Usart, uint8_t RxPad)
{
	SERCOM_UsartDisable(Usart);
	Usart->CTRLA.bit.RXPO = RxPad;
	SERCOM_UsartEnable(Usart);
}

extern void SERCOM_EnableClock(uint8_t Instance);
extern void SERCOM_UsartInit(uint8_t Instance, uint32_t CtrlA, uint32_t CtrlB, uint32_t CtrlC, uint16_t Baud);
extern void SERCOM_ConfigurePios(uint8_t Instance, uint8_t Mask);

#endif /* SERCOM_H_ */