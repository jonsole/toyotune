#ifndef ESP_H_
#define ESP_H_

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
//#include <stdio.h>

#include "esp_hw.h"
#include "debug.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_PKT_HEADER_SIZE	(2)
#define ESP_SYNC_PKT_SIZE	(1)
#define ESP_ACK_PKT_SIZE	(2)

/* ESP Packet Header
 
 Byte  Bit  Description
 0     7:6  Channel - 0 Link Control
					  1,2,3 - Application specific
	   5:3  Tx Sequence Number - 0:7  
	   2:0  Next Expected Seq. Num. - 0:7 

 1     7    CRC Present 0 = No CRC
					    1 = CRC at end of packet
       6:0  Payload length 0:127
*/

#define ESP_PKT_CHANNEL_POS (6)
#define ESP_PKT_CHANNEL_MSK (0x03 << ESP_PKT_CHANNEL_POS)

#define ESP_PKT_SEQ_POS (3)
#define ESP_PKT_SEQ_MSK (0x07 << ESP_PKT_SEQ_POS)

#define ESP_PKT_ACK_POS (0)
#define ESP_PKT_ACK_MSK (0x07 << ESP_PKT_ACK_POS)

#define ESP_PKT_CRC_PRESENT_POS (7)
#define ESP_PKT_CRC_PRESENT_MSK (0x01 << ESP_PKT_CRC_PRESENT_POS)

#define ESP_PKT_PAYLOAD_SIZE_POS (0)
#define ESP_PKT_PAYLOAD_SIZE_MSK (0x7F << ESP_PKT_PAYLOAD_SIZE_POS)

#define ESP_CHANNEL_LC (0)



struct ESP;

typedef enum
{
	ESP_PACKET_TYPE_SYNC,
	ESP_PACKET_TYPE_ACK,
	ESP_PACKET_TYPE_PAYLOAD_STATIC,
	ESP_PACKET_TYPE_PAYLOAD_DYNAMIC	
} ESP_Packet_Type_t;

typedef struct ESP_Packet
{ 
	struct ESP_Packet *Next;
	ESP_Packet_Type_t Type:2;
	bool SeqNumberValid:1;
	uint8_t Header[ESP_PKT_HEADER_SIZE];
	uint8_t *Data;
} ESP_Packet_t;

#define ESP_TIMER_NOW  (0)
#define ESP_TIMER_IDLE (0xFFFFU)

typedef enum
{
	ESP_LC_STATE_INIT,
	ESP_LC_STATE_WAIT_RES,
	ESP_LC_STATE_IND_WAIT_RES,
	ESP_LC_STATE_ACTIVE
} Esp_LcState_t;


typedef enum
{
	ESP_SYNC_STATE_SHY,
	ESP_SYNC_STATE_CURIOUS,
	ESP_SYNC_STATE_GARRULOUS,
} Esp_SyncState_t;

#define ESP_SYNC_PACKET_SYNC		(0x55)
#define ESP_SYNC_PACKET_SYNC_RESP	(0xAA)
#define ESP_SYNC_PACKET_CONF		(0xA5)
#define ESP_SYNC_PACKET_CONF_RESP	(0x5A)
#define ESP_SYNC_PACKET_KEEP_ALIVE	(0x99)

typedef struct ESP
{
	ESP_Hardware_t Hw;

	uint8_t Instance;
	Esp_SyncState_t SyncState;
	uint16_t SyncTimer;
	uint8_t SyncKeepAlive;
	
	// Rx State
	uint8_t RxAck;					// Last acknowledgment received
	uint8_t RxSeq;					// Expected sequence number to receive in packet
	uint8_t RxWindow;				// Receive window size (set from peer)
	uint16_t RxAckTimer;			// Time in milliseconds to left wait for acknowledgment
	uint16_t RxRetransmitPeriod;
	uint8_t RxByte;
	ESP_Packet_t *RxPacket;

	uint8_t RxPacketDataSize;
	uint8_t RxPacketDataIndex;

	// Tx State
	uint8_t TxAck;					// Acknowledgment to send in packet
	uint8_t TxSeq;					// Sequence number of next transmitted packet
	uint8_t TxWindow;				// Transmit window size
	uint16_t TxAckTimer;			// Timer in milliseconds to send acknowledgment (0 == no timer)
	uint16_t TxRetransmitPeriod;
	ESP_Packet_t *TxPacketAckList;	// Packets awaiting acknowledgment
	ESP_Packet_t *TxPacketList;		// Packets waiting to be transmitted
	ESP_Packet_t *TxPacket;			// Packet currently being transmitted

	uint8_t TxPacketDataSize;		// Amount of data to be encoded
	uint8_t TxPacketDataIndex;
} ESP_t;

#define ESP_Debug(Esp, x, ...) Debug("ESP%d: " x, Esp->Instance, ##__VA_ARGS__)

extern void ESP_Init(ESP_t *Esp, uint8_t Instance, uint8_t DmaChannel, uint8_t Timer);
extern void ESP_TxInit(ESP_t *Esp);
extern void ESP_RxInit(ESP_t *Esp);

extern void ESP_LinkActive(ESP_t *Esp);
extern void ESP_LinkReset(ESP_t *Esp);

extern void ESP_Task(ESP_t *Esp);
extern void ESP_SyncTask(ESP_t *Esp);
extern void ESP_TimerTick(ESP_t *Esp);

extern void ESP_RxReset(ESP_t *Esp);
extern void ESP_RxTask(ESP_t *Esp);

extern void ESP_TxReset(ESP_t *Esp);
extern void ESP_TxTask(ESP_t *Esp);
extern void ESP_TxHandleAcknowledgment(ESP_t *Esp, const uint8_t Ack);
extern void ESP_TxPacket(ESP_t *Esp, ESP_Packet_t *TxPacket);

extern ESP_Packet_t *ESP_CreatePacket(const uint8_t Channel, void *Data, uint8_t DataSize, bool IsDataStatic);
extern void ESP_DestroyPacket(ESP_Packet_t *Packet);

//extern void ESP_LcInit(ESP_t *Esp);
//extern ESP_Packet_t *ESP_LcAllocatePacket(ESP_t *Esp, uint8_t PayloadSize);
//extern ESP_Packet_t *ESP_LcCreateConfInd(ESP_t *Esp, uint8_t RxWindow, uint8_t RxMtu);
//extern ESP_Packet_t *ESP_LcCreateConfRes(ESP_t *Esp, uint8_t RxWindow, uint8_t RxMtu);
//extern void ESP_LcHandleRxPacket(ESP_t *Esp, ESP_Packet_t *Packet);

extern void CALLBACK_ESP_LinkActive(ESP_t *Esp);
extern void CALLBACK_ESP_LinkReset(ESP_t *Esp);
extern void CALLBACK_ESP_PacketReceived(ESP_t *Esp, uint8_t *Packet, uint8_t PacketSize);

extern void ESP_SyncRxPacket(ESP_t *Esp, uint8_t Packet);

static inline bool ESP_IsSynced(ESP_t *Esp)
{
	return (Esp->SyncState == ESP_SYNC_STATE_GARRULOUS);	
}

#define ESP_SLIP_FRAME			(0xC0)
#define ESP_SLIP_ESCAPE			(0xDB)
#define ESP_SLIP_ESCAPE_FRAME	(0xDC)
#define ESP_SLIP_ESCAPE_ESCAPE	(0xDD)

#define ESP_SIGNAL_RX_IDLE (1 << OS_SIGNAL_USER)
#endif /* ESP_H_ */