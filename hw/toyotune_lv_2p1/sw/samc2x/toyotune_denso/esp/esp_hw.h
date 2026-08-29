/*
 * esp_hw.h
 *
 * Created: 21/05/2016 21:44:29
 *  Author: Jon
 */ 


#ifndef ESP_HW_H_
#define ESP_HW_H_

#include <sam.h>

#include "buffer.h"


struct ESP;

#define ESP_RX_BUFFER_SIZE (1024)
#define ESP_TX_BUFFER_SIZE (1024)

typedef struct
{
	uint16_t Index;
	uint16_t Outdex;
	uint8_t Buffer[ESP_RX_BUFFER_SIZE];
} ESP_RxBuffer_t;

typedef struct
{
	uint16_t Index;
	uint16_t Outdex;
	uint8_t Buffer[ESP_TX_BUFFER_SIZE];
} ESP_TxBuffer_t;

typedef struct ESP_Hardware
{
	SercomUsart *Usart;

	ESP_RxBuffer_t RxUsartBuffer;
	ESP_TxBuffer_t TxUsartBuffer;

	uint8_t RxUsartDmaChannel;
	uint8_t TxUsartDmaChannel;
	uint8_t TxUsartDmaTrigger;
	uint8_t RxUsartDmaTrigger;
} ESP_Hardware_t;

extern void ESP_HwInit(struct ESP *Esp, uint8_t DmaChannel, uint8_t Timer);
extern void ESP_HwTask(struct ESP *Esp);
extern void ESP_HwTxKick(struct ESP *Esp);

static uint16_t __inline ESP_HwTxBufferIndex(ESP_Hardware_t *Hw)
{
	return BufferIndex(Hw->TxUsartBuffer);
}

static uint16_t __inline ESP_HwTxBufferSpace(ESP_Hardware_t *Hw)
{
	return BufferSpace(Hw->TxUsartBuffer);
}

static void __inline ESP_HwTxBufferWrite(ESP_Hardware_t *Hw, uint8_t Data)
{
	BufferWrite(Hw->TxUsartBuffer, Data);
}

static bool __inline ESP_HwRxBufferIsEmpty(ESP_Hardware_t *Hw)
{
	return BufferIsEmpty(Hw->RxUsartBuffer);
}

static uint8_t __inline ESP_HwRxBufferRead(ESP_Hardware_t *Hw)
{
	return BufferRead(Hw->RxUsartBuffer);
}

extern uint8_t ESP_HwGetUsartInstance(SercomUsart *Usart);

extern void ESP_HwRxDmaSync(struct ESP *Esp);
extern void ESP_HwRxDmaStart(struct ESP *Esp);
extern void ESP_HwTxDmaSync(struct ESP *Esp);

#endif /* ESP_HW_H_ */