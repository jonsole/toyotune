/*
 * dmac.h
 *
 * Created: 02/06/2016 19:58:32
 *  Author: Jon
 */ 


#ifndef DMAC_H_
#define DMAC_H_

#include <sam.h>

struct DMAC_Descriptor_t
{
	__IO DMAC_BTCTRL_Type          BTCTRL;      /**< \brief Offset: 0x00 (R/W 16) Block Transfer Control */
	__IO DMAC_BTCNT_Type           BTCNT;       /**< \brief Offset: 0x02 (R/W 16) Block Transfer Count */
	__IO DMAC_SRCADDR_Type         SRCADDR;     /**< \brief Offset: 0x04 (R/W 32) Block Transfer Source Address */
	__IO DMAC_DSTADDR_Type         DSTADDR;     /**< \brief Offset: 0x08 (R/W 32) Block Transfer Destination Address */
	__IO DMAC_DESCADDR_Type        DESCADDR;    /**< \brief Offset: 0x0C (R/W 32) Next Descriptor Address */
} __attribute__((packed));

typedef struct DMAC_Descriptor_t DMAC_Descriptor_t;

#define DMAC_NO_CHANNEL	(0xFF)

extern void DMAC_Init(void);
extern uint8_t DMAC_ChannelAllocate(void (*InterruptHandler)(void *, const uint8_t, const uint16_t), void *InterruptData, uint8_t RequestedChannel);
extern void DMAC_ChannelFree(uint8_t Channel);
extern DMAC_Descriptor_t *DMAC_ChannelGetBaseDescriptor(uint8_t Channel);
extern DMAC_Descriptor_t *DMAC_ChannelGetWriteBackDescriptor(uint8_t Channel);
extern uint16_t DMAC_ChannelGetByteCount(uint8_t Channel);


#endif /* DMAC_H_ */