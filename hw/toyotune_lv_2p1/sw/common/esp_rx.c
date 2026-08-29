#include "esp.h"
#include "mem.h"

#define ESP_RxDebug(...) //Debug(__VA_ARGS__)

/* Validate ESP header */
static void ESP_RxHeaderValidate(ESP_t *Esp)
{
	const uint8_t Channel = (Esp->RxPacket->Header[0] & ESP_PKT_CHANNEL_MSK) >> ESP_PKT_CHANNEL_POS;
	const uint8_t Seq = (Esp->RxPacket->Header[0] & ESP_PKT_SEQ_MSK) >> ESP_PKT_SEQ_POS; 
	const uint8_t Ack = (Esp->RxPacket->Header[0] & ESP_PKT_ACK_MSK) >> ESP_PKT_ACK_POS;
	const uint8_t PayloadSize = (Esp->RxPacket->Header[1] & ESP_PKT_PAYLOAD_SIZE_MSK) >> ESP_PKT_PAYLOAD_SIZE_POS;

	/* Pass received acknowledgment to transmitter */
	Esp->RxAck = Ack;
	ESP_TxHandleAcknowledgment(Esp, Ack);

	if (PayloadSize != 0)
	{		 
		/* Check if sequence number is as expected */
		if (Seq == Esp->RxSeq)
		{
			ESP_Debug(Esp, "Rx Packet, Channel %d, Seq %d, Ack %d, Payload %d\n", Channel, Seq, Ack, PayloadSize);

			/* Advance expected sequence number */
			Esp->RxSeq = (Esp->RxSeq + 1) & 0x07;

			/* Check distance from sent acknowledgment */
			const uint8_t Distance = (Esp->RxSeq - Esp->TxAck) & 0x07;
			ESP_Debug(Esp, "Rx Distance %d, RxSeq %d, TxAck %d\n", Distance, Esp->RxSeq, Esp->TxAck);
			if (Distance >= Esp->RxWindow)
			{
				/* Set acknowledgment to transmit */
				Esp->TxAck = Esp->RxSeq;
				ESP_Debug(Esp, "TxAck %d\n", Esp->TxAck);

				/* Send acknowledgment straightaway */
				Esp->TxAckTimer = ESP_TIMER_NOW;
			}
			else
			{
				/* Check if acknowledgment timer is not running */
				if (Esp->TxAckTimer == ESP_TIMER_IDLE)
				{
					/* Start acknowledgment timer, to send acknowledgment before peers retransmit timer expires */
					Esp->TxAckTimer = Esp->RxRetransmitPeriod / 2;
				}
			}

			/* Allocate payload */
			Esp->RxPacket->Data = MEM_Alloc(PayloadSize);
			if (Esp->RxPacket->Data)
			{
				Esp->RxPacket->Type = ESP_PACKET_TYPE_PAYLOAD_DYNAMIC;
				Esp->RxPacketDataSize = PayloadSize + ESP_PKT_HEADER_SIZE;
			}
		}
		else
		{
			ESP_Debug(Esp, "Rx Packet (OOS), Channel %d, Seq %d (Expected %d), Ack %d, Payload %d\n", Channel, Seq, Esp->RxSeq, Ack, PayloadSize);

			/* Sequence number wasn't as expected, send expected sequence number as acknowledgment */
			Esp->TxAck = Esp->RxSeq;
			ESP_Debug(Esp, "TxAck %d\n", Esp->TxAck);

			/* Send acknowledgment straightaway */
			Esp->TxAckTimer = ESP_TIMER_NOW;
		}
	}
}


void ESP_RxPacket(ESP_t *Esp)
{
	if (Esp->RxPacket)
		ESP_DestroyPacket(Esp->RxPacket);
		
	ESP_Packet_t *Packet = MEM_Create(ESP_Packet_t);

	Packet->Next = NULL;
	Packet->Type = ESP_PACKET_TYPE_PAYLOAD_STATIC;
	Packet->Data = NULL;
	Packet->SeqNumberValid = true;
	
	Packet->Header[0] = 0x00;
	Packet->Header[1] = 0x00;

	Esp->RxPacket = Packet;
	
	Esp->RxPacketDataIndex = 0;
	Esp->RxPacketDataSize = ESP_PKT_HEADER_SIZE;
}

void ESP_RxInit(ESP_t *Esp)
{
	Esp->RxAck = 0;
	Esp->RxSeq = 0;
	Esp->RxWindow = 1;
	Esp->RxAckTimer = ESP_TIMER_IDLE;
	Esp->RxRetransmitPeriod = 5000;
	Esp->RxPacket = NULL;
	
	ESP_RxPacket(Esp);
}

void ESP_RxReset(ESP_t *Esp)
{
	ESP_RxInit(Esp);		
}

void ESP_RxTask(ESP_t *Esp)
{
	while (!ESP_HwRxBufferIsEmpty(&Esp->Hw))
	{
		uint8_t RxByte = ESP_HwRxBufferRead(&Esp->Hw);
		ESP_RxDebug("%02x:", RxByte);
		
		if (RxByte == ESP_SLIP_ESCAPE)
			Esp->RxByte = RxByte;
		else if (RxByte == ESP_SLIP_FRAME)
		{
			Esp->RxByte = RxByte;
			
			/* Check if we received sync packet */
			if (Esp->RxPacketDataIndex == ESP_SYNC_PKT_SIZE)
				ESP_SyncRxPacket(Esp, Esp->RxPacket->Header[0]);
			/* Check we got complete packet */
			else if (Esp->RxPacketDataIndex == Esp->RxPacketDataSize)
			{
				ESP_Debug(Esp,"Rx complete packet, %02x %02x\n", Esp->RxPacket->Data[0], Esp->RxPacket->Data[1]);
				CALLBACK_ESP_PacketReceived(Esp, Esp->RxPacket->Data, Esp->RxPacketDataSize - ESP_PKT_HEADER_SIZE);
				Esp->RxPacket->Data = NULL;
			}
			
			/* Initialise receiver for next packet */
			ESP_RxPacket(Esp);
		}
		else
		{
			if (Esp->RxByte == ESP_SLIP_ESCAPE)
			{
				Esp->RxByte = RxByte;
				if (RxByte == ESP_SLIP_ESCAPE_FRAME)
					RxByte = ESP_SLIP_FRAME;
				else if (RxByte == ESP_SLIP_ESCAPE_ESCAPE)
					RxByte = ESP_SLIP_ESCAPE;
			}
			else
				Esp->RxByte = RxByte;

			/* Check we have a buffer to write into */
			if (Esp->RxPacketDataIndex < Esp->RxPacketDataSize)
			{
				/* Write byte into buffer */
				if (Esp->RxPacketDataIndex < ESP_PKT_HEADER_SIZE)
					Esp->RxPacket->Header[Esp->RxPacketDataIndex] = RxByte;
				else
					Esp->RxPacket->Data[Esp->RxPacketDataIndex - ESP_PKT_HEADER_SIZE] = RxByte;				
				Esp->RxPacketDataIndex += 1;
				
				/* Check if we have received header */
				if (Esp->RxPacketDataIndex == ESP_PKT_HEADER_SIZE)
				{
					/* Validate header and allocate packet for payload */
					ESP_RxHeaderValidate(Esp);
				}
			}
			else
				ESP_Debug(Esp, "Packet too large\n");
		}
	}
}
