/*
 * esp_sync.c
 *
 * Created: 07/11/2021 16:50:12
 *  Author: Jon
 */ 

#include "esp.h"
#include "mem.h"

#define ESP_SYNC_PERIOD	(100)
#define ESP_SYNC_KEEP_ALIVE_PERIOD (500)

static void ESP_SyncTxPacket(ESP_t *Esp, uint8_t Type)
{
	ESP_Packet_t *Packet = MEM_Create(ESP_Packet_t);
	Packet->Type = ESP_PACKET_TYPE_SYNC;
	Packet->Next = NULL;
	Packet->Header[0] = Type;
	ESP_TxPacket(Esp, Packet);
}


void ESP_SyncTask(ESP_t *Esp)
{
	if (Esp->SyncState != ESP_SYNC_STATE_GARRULOUS)
	{
		if (Esp->SyncTimer == ESP_TIMER_NOW)
		{
			switch (Esp->SyncState)
			{
				case ESP_SYNC_STATE_SHY:
					ESP_Debug(Esp, "Sending SYNC\n");
					ESP_SyncTxPacket(Esp, ESP_SYNC_PACKET_SYNC);
					Esp->SyncTimer = ESP_SYNC_PERIOD;
					break;
				
				case ESP_SYNC_STATE_CURIOUS:
					ESP_Debug(Esp, "Sending CONF\n");
					ESP_SyncTxPacket(Esp, ESP_SYNC_PACKET_CONF);
					Esp->SyncTimer = ESP_SYNC_PERIOD;
					break;
					
				default:
					Esp->SyncTimer = ESP_TIMER_IDLE;
					break;
			}
		}
	}
	else
	{
		if (Esp->SyncTimer == ESP_TIMER_NOW)
		{
			/* Send keep alive */
			ESP_SyncTxPacket(Esp, ESP_SYNC_PACKET_KEEP_ALIVE);
			Esp->SyncTimer = ESP_SYNC_KEEP_ALIVE_PERIOD;
			Esp->SyncKeepAlive += 1;
			if (Esp->SyncKeepAlive >= 4)
			{
				ESP_Debug(Esp, "No Keep alive received\n");
				ESP_LinkReset(Esp);				
			}
		}
	}	
}


void ESP_SyncRxPacket(ESP_t *Esp, uint8_t Packet)
{
	//ESP_Debug(Esp, "Rx Sync Packet %02x, state %u\n", Packet, Esp->SyncState);

	switch (Esp->SyncState)
	{
		case ESP_SYNC_STATE_SHY:
		{
			if (Packet == ESP_SYNC_PACKET_SYNC)
			{
				ESP_Debug(Esp, "Rx SYNC, sending SYNC_RESP\n");
				ESP_SyncTxPacket(Esp, ESP_SYNC_PACKET_SYNC_RESP);
			}
			else if (Packet == ESP_SYNC_PACKET_SYNC_RESP)
			{
				ESP_Debug(Esp, "Rx CONF_RESP, moving to Curious state\n");
				Esp->SyncState = ESP_SYNC_STATE_CURIOUS;
				Esp->SyncTimer = ESP_TIMER_NOW;
			}
		}
		break;

		case ESP_SYNC_STATE_CURIOUS:
		{
			if (Packet == ESP_SYNC_PACKET_SYNC)
			{
				ESP_Debug(Esp, "Rx SYNC, sending SYNC_RESP\n");
				ESP_SyncTxPacket(Esp, ESP_SYNC_PACKET_SYNC_RESP);
			}
			else if (Packet == ESP_SYNC_PACKET_CONF)
			{
				ESP_Debug(Esp, "Rx CONF, sending CONF_RESP\n");
				ESP_SyncTxPacket(Esp, ESP_SYNC_PACKET_CONF_RESP);
			}
			else if (Packet == ESP_SYNC_PACKET_CONF_RESP)
			{
				ESP_Debug(Esp, "Rx CONF_RESP, moving to Garrulous state\n");
				Esp->SyncState = ESP_SYNC_STATE_GARRULOUS;
				Esp->SyncTimer = ESP_SYNC_KEEP_ALIVE_PERIOD;	
				Esp->SyncKeepAlive = 0;			
				ESP_LinkActive(Esp);
			}
		}
		break;
		
		case ESP_SYNC_STATE_GARRULOUS:
		{
			if (Packet == ESP_SYNC_PACKET_CONF)
			{
				ESP_Debug(Esp, "Rx CONF, sending CONF_RESP\n");
				ESP_SyncTxPacket(Esp, ESP_SYNC_PACKET_CONF_RESP);
			} 
			else if (Packet == ESP_SYNC_PACKET_SYNC)
			{
				ESP_Debug(Esp, "Rx SYNC, link reset\n");
				ESP_LinkReset(Esp);
			}
			else if (Packet == ESP_SYNC_PACKET_KEEP_ALIVE)
			{
				ESP_Debug(Esp, "Rx KEEP_ALIVE, link active\n");				
				Esp->SyncKeepAlive = 0;
			}
		}
		break;
		
		default:
			break;
	}
}