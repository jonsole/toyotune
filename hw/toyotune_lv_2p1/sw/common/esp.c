//#include <LUFA/Common/Common.h>

#include "esp.h"
#include "mem.h"
#include "sercom.h"
#include <stdbool.h>

void ESP_SyncInit(ESP_t *Esp)
{
	Esp->SyncState = ESP_SYNC_STATE_SHY;
	Esp->SyncTimer = 100;
}

void ESP_Init(ESP_t *Esp, uint8_t Instance, uint8_t DmaChannel, uint8_t Timer)
{
	Esp->Hw.Usart = &SERCOM_GetSercom(Instance)->USART;
	Esp->Instance = Instance;

	ESP_RxInit(Esp);
	ESP_TxInit(Esp);
	ESP_SyncInit(Esp);
	ESP_HwInit(Esp, DmaChannel, Timer);

}

void ESP_LinkReset(ESP_t *Esp)
{
	CALLBACK_ESP_LinkReset(Esp);	
	ESP_RxReset(Esp);
	ESP_TxReset(Esp);
	ESP_SyncInit(Esp);
}

void ESP_LinkActive(ESP_t *Esp)
{
	CALLBACK_ESP_LinkActive(Esp);	
}

void ESP_Task(ESP_t *Esp)
{
	ESP_HwTask(Esp);
	ESP_RxTask(Esp);
	ESP_TxTask(Esp);
	ESP_SyncTask(Esp);
}

void ESP_TimerTick(ESP_t *Esp)
{
	/* Check transmit acknowledgment timer is active and hasn't expired */
	if ((Esp->TxAckTimer != ESP_TIMER_IDLE) &&
		(Esp->TxAckTimer != ESP_TIMER_NOW))
	{
		/* Decrement timer */
		Esp->TxAckTimer--;
	}

	/* Check receive acknowledgment timer is active and hasn't expired */
	if ((Esp->RxAckTimer != ESP_TIMER_IDLE) &&
		(Esp->RxAckTimer != ESP_TIMER_NOW))
	{
		/* Decrement timer */
		Esp->RxAckTimer--;
	}

	/* Check transmit sync timer is active and hasn't expired */
	if ((Esp->SyncTimer != ESP_TIMER_IDLE) &&
		(Esp->SyncTimer != ESP_TIMER_NOW))
	{
		/* Decrement timer */
		Esp->SyncTimer--;
	}
}

ESP_Packet_t *ESP_CreatePacket(const uint8_t Channel, void *Data, uint8_t DataSize, bool IsDataStatic)
{
	ESP_Packet_t *Packet = MEM_Create(ESP_Packet_t);
	if (Packet)
	{
		Packet->Next = NULL;
		Packet->Type = IsDataStatic ? ESP_PACKET_TYPE_PAYLOAD_STATIC : ESP_PACKET_TYPE_PAYLOAD_DYNAMIC;
		Packet->Data = Data;
		Packet->SeqNumberValid = false;
	
		Packet->Header[0] = Channel << ESP_PKT_CHANNEL_POS;
		Packet->Header[1] = DataSize << ESP_PKT_PAYLOAD_SIZE_POS;
	}
	return Packet;
}


void ESP_DestroyPacket(ESP_Packet_t *Packet)
{	
	if (Packet)
	{
		if (Packet->Type == ESP_PACKET_TYPE_PAYLOAD_DYNAMIC && Packet->Data)
			MEM_Free(Packet->Data);
	
		MEM_Free(Packet);
	}
}

