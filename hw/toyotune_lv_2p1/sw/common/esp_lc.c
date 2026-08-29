#include "esp.h"
#include "mem.h"

 #define ESP_PKT_LC_CONF_IND	(0xA0)
 typedef struct  
 {
	uint8_t Id;
	uint8_t RxWindow;
	uint8_t RxMtu;
 } __attribute__ ((packed)) ESP_LcConfInd_t;

 #define ESP_PKT_LC_CONF_RES	(0xA1)
 typedef struct  
 {
	uint8_t Id;
	uint8_t RxWindow;
	uint8_t RxMtu;
 } __attribute__ ((packed)) ESP_LcConfRes_t;
 
 #define ESP_PKT_LC_PING_REQ	(0xB0)
 #define ESP_PKT_LC_PING_CFM	(0xB1)


ESP_Packet_t *ESP_LcCreateConfInd(ESP_t *Esp, uint8_t RxWindow, uint8_t RxMtu)
{
	ESP_Packet_t *Packet = ESP_LcAllocatePacket(Esp, sizeof(ESP_LcConfRes_t));
	ESP_LcConfInd_t *ConfInd = (ESP_LcConfInd_t *)Packet->Data;
	ConfInd->Id = ESP_PKT_LC_CONF_IND;
	ConfInd->RxWindow = RxWindow;
	ConfInd->RxMtu = RxMtu;
	return Packet;
}

ESP_Packet_t *ESP_LcCreateConfRes(ESP_t *Esp, uint8_t RxWindow, uint8_t RxMtu)
{
	ESP_Packet_t *Packet = ESP_LcAllocatePacket(Esp, sizeof(ESP_LcConfRes_t));
	ESP_LcConfRes_t *ConfRes = (ESP_LcConfRes_t *)Packet->Data;
	ConfRes->Id = ESP_PKT_LC_CONF_RES;
	ConfRes->RxWindow = RxWindow;
	ConfRes->RxMtu = RxMtu;
	return Packet;
}

void ESP_LcHandleConfInd(ESP_t *Esp, ESP_LcConfInd_t *ConfInd, uint8_t DataSize)
{
	ESP_Debug(Esp, "LcHandleConfInd\n");
	if (Esp->LcGotConfRes && !Esp->LcGotConfInd)
	{
		ESP_Debug(Esp, "Link active\n");

		Esp->LcIsActive = 1;
		CALLBACK_ESP_LinkActive(Esp);
	}
	Esp->LcGotConfInd = 1;

	/* Send LC_CONF_RES */
	ESP_TxPacket(Esp, ESP_LcCreateConfRes(Esp, ConfInd->RxWindow, ConfInd->RxMtu));
}

void ESP_LcHandleConfRes(ESP_t *Esp, ESP_LcConfRes_t *ConfRes, uint8_t DataSize)
{
	ESP_Debug(Esp, "LcHandleConfRes\n");
	if (!Esp->LcGotConfRes && Esp->LcGotConfInd)
	{
		ESP_Debug(Esp, "Link active\n");

		Esp->LcIsActive = 1;
		CALLBACK_ESP_LinkActive(Esp);
	}
	Esp->LcGotConfRes = 1;
}

void ESP_LcHandleRxPacket(ESP_t *Esp, ESP_Packet_t *Packet)
{
	ESP_Debug(Esp, "LcValidateRxPacket, ID %02x\n", Packet->Data[0]);

	uint8_t Id = Packet->Data[0];
	switch (Id)
	{
		case ESP_PKT_LC_CONF_IND:
			ESP_LcHandleConfInd(Esp, (ESP_LcConfInd_t *)Packet->Data, Packet->DataSize);
			break;
			
		case ESP_PKT_LC_CONF_RES:
			ESP_LcHandleConfRes(Esp, (ESP_LcConfRes_t *)Packet->Data, Packet->DataSize);
			break;
	}
}


ESP_Packet_t *ESP_LcAllocatePacket(ESP_t *Esp, uint8_t PayloadSize)
{
	ESP_Packet_t *Packet = MEM_Alloc(sizeof(ESP_Packet_t) + PayloadSize);
	return ESP_CreatePacket(ESP_CHANNEL_LC, (uint8_t *)(Packet + 1), PayloadSize, true);
}

void ESP_LcInit(ESP_t *Esp)
{
	Esp->LcIsActive = 0;
	Esp->LcGotConfInd = Esp->LcGotConfRes = 0;

	//ESP_TxPacket(Esp, ESP_LcCreateConfInd(Esp, 7, 255));
}