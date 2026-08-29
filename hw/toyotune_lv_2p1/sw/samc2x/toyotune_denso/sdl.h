/*
 * sdl.h
 *
 * Created: 19/10/2016 20:16:04
 *  Author: Jon
 */ 

#ifndef SDL_H_
#define SDL_H_

#include <sam.h>
#include <stdbool.h>

typedef struct SDL
{
	SercomUsart *Usart;
	uint8_t Instance;
	uint8_t DmaSize;
	uint8_t DmaBufferIndex;
	uint8_t *DmaBuffer[2];
	uint8_t RxUsartDmaChannel;
	uint8_t RxUsartDmaTrigger;
	void (*Callback)(struct SDL *, void *, const uint8_t *, uint8_t);
	void *CallbackData;
} SDL_t;


extern void SDL_Init(SDL_t *Sdl, uint8_t DmaChannel, uint8_t Instance, uint8_t RxPad, uint8_t DmaSize, void (*Callback)(struct SDL *, void *, const uint8_t *, uint8_t), void *CallbackData);

#endif /* SDL_H_ */