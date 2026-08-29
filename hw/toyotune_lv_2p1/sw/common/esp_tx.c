#include "mem.h"
#include "esp.h"
#include "debug.h"

#define ESP_TxDebug(...)

bool ESP_TxEncodeBytes(ESP_t *Esp)
{
	const uint16_t BufIndex = ESP_HwTxBufferIndex(&Esp->Hw);
	bool TxComplete = false;

	while (ESP_HwTxBufferSpace(&Esp->Hw) >= 3)
	{
		/* Check if we are at start of packet */
		if (Esp->TxPacketDataIndex == 0)
		{
			/* Send frame byte */
			ESP_HwTxBufferWrite(&Esp->Hw, ESP_SLIP_FRAME);
			ESP_TxDebug("%02x:", ESP_SLIP_FRAME);
		}
		/* Check if we are at end of packet */
		else if (Esp->TxPacketDataIndex == Esp->TxPacketDataSize)
		{
			/* Send frame byte */
			ESP_HwTxBufferWrite(&Esp->Hw, ESP_SLIP_FRAME);
			ESP_TxDebug("%02x\n", ESP_SLIP_FRAME);
			TxComplete = true;

			/* Exit loop */
			break;
		}
		
		/* Get byte of data from header or payload */
		const uint8_t Byte = (Esp->TxPacketDataIndex < ESP_PKT_HEADER_SIZE) ?
							 Esp->TxPacket->Header[Esp->TxPacketDataIndex] :
							 Esp->TxPacket->Data[Esp->TxPacketDataIndex - ESP_PKT_HEADER_SIZE];
		Esp->TxPacketDataIndex += 1;
		
		/* Check if we need to escape byte */
		if (Byte == ESP_SLIP_FRAME || Byte == ESP_SLIP_ESCAPE)
		{
			ESP_HwTxBufferWrite(&Esp->Hw, ESP_SLIP_ESCAPE);
			ESP_HwTxBufferWrite(&Esp->Hw, (Byte == ESP_SLIP_FRAME) ? ESP_SLIP_ESCAPE_FRAME : ESP_SLIP_ESCAPE_ESCAPE);
			ESP_TxDebug("%02x:02x:", ESP_SLIP_ESCAPE, (Byte == ESP_SLIP_FRAME) ? ESP_SLIP_ESCAPE_FRAME : ESP_SLIP_ESCAPE_ESCAPE);
		}
		else
		{
			ESP_HwTxBufferWrite(&Esp->Hw, Byte);
			ESP_TxDebug("%02x:", Byte);
		}
	}
	
	/* Check if buffer index has changed */
	if (ESP_HwTxBufferIndex(&Esp->Hw) != BufIndex)
	{
		/* Kick USART Tx DMA */
		ESP_HwTxKick(Esp);
	}
	
	/* Return true if packet completed */
	return TxComplete;
}

void ESP_TxRestartRxAckTimer(ESP_t *Esp)
{
	/* Start timer if packets to be acknowledged, or stop
	   timer if no packets to be acknowledged */
	if (Esp->TxPacketAckList)
		Esp->RxAckTimer = Esp->TxRetransmitPeriod;
	else
		Esp->RxAckTimer = ESP_TIMER_IDLE;
}

void ESP_TxHandleAcknowledgment(ESP_t *Esp, const uint8_t Ack)
{
	ESP_Packet_t *Packet = Esp->TxPacketAckList;

	/* Walk down list of packets waiting acknowledgment */
	while (Packet)
	{
		/* Check if acknowledgment is the sequence number after this packet */
		const uint8_t PacketSeq = (Packet->Header[0] & ESP_PKT_SEQ_MSK) >> ESP_PKT_SEQ_POS;
		const uint8_t NextSeq = (PacketSeq + 1) & 0x07;
		if (NextSeq == Ack)
		{
			/* Remove all packets in list up to and including this packet */
			ESP_Packet_t *AckPacket = Packet->Next;
			while (Esp->TxPacketAckList != AckPacket)
			{
				/* Get packet at head list */
				Packet = Esp->TxPacketAckList;

				ESP_Debug(Esp, "Packet ACK'd %d\n", (Packet->Header[0] & ESP_PKT_SEQ_MSK) >> ESP_PKT_SEQ_POS);

				/* Advance list to next packet */
				Esp->TxPacketAckList = Esp->TxPacketAckList->Next;

				/* Destroy packet that was at head of list */
				ESP_DestroyPacket(Packet);
			}

			/* Start receive acknowledgment timer */
			ESP_TxRestartRxAckTimer(Esp);

			/* Exit loop we've removed all acknowledged packets */			
			break;			
		}

		/* Try next packet */
		Packet = Packet->Next;
	}
}

static void ESP_TxEncodePacket(ESP_t *Esp)
{
	if (Esp->TxPacket)
	{
		/* Encode packet */
		if (ESP_TxEncodeBytes(Esp))
		{
			/* Complete packet encoded, check if payload packet (not just an ACK) */
			if (Esp->TxPacket->Type == ESP_PACKET_TYPE_PAYLOAD_DYNAMIC || Esp->TxPacket->Type == ESP_PACKET_TYPE_PAYLOAD_STATIC)
			{
				/* Find end of awaiting acknowledgment list */
				ESP_Packet_t *Packet = Esp->TxPacketAckList;
				if (Packet)
				{
					while (Packet->Next)
						Packet = Packet->Next;

					/* Add packet to end of awaiting acknowledgment list */
					Packet->Next = Esp->TxPacket;
				}
				else
				{
					/* Nothing on awaiting acknowledgment list */
					Esp->TxPacketAckList = Esp->TxPacket;
				}

				if (Esp->RxAckTimer == ESP_TIMER_IDLE)
					ESP_TxRestartRxAckTimer(Esp);

				/* Terminate list */
				Esp->TxPacket->Next = NULL;
			}
			else
				ESP_DestroyPacket(Esp->TxPacket);			

			/* No packet being transmitted */
			Esp->TxPacket = NULL;
		}
	}
}


void ESP_TxTask(ESP_t *Esp)
{
	/* Check if packet is being transmitted */
	if (Esp->TxPacket)
	{
		/* SLIP encode chunks until there's no space in buffer or there's no more chunks */
		ESP_TxEncodePacket(Esp);
	}
	/* Check transmit acknowledgment timer has expired */
	else if (Esp->TxAckTimer == ESP_TIMER_NOW)
	{
		ESP_Debug(Esp, "Sending ACK %d\n", Esp->TxAck);

		/* Stop timer */
		Esp->TxAckTimer = ESP_TIMER_IDLE;

		/* Create acknowledgment only packet */
		ESP_Packet_t *Packet = MEM_Create(ESP_Packet_t);
		Packet->Type = ESP_PACKET_TYPE_ACK;
		Packet->Next = NULL;
		Packet->Header[0] = Esp->TxAck << ESP_PKT_ACK_POS;
		Packet->Header[1] = 0x00;
		ESP_TxPacket(Esp, Packet);
	}

	/* Check receive acknowledgment timer has expired */
	else if (Esp->RxAckTimer == ESP_TIMER_NOW)
	{
		ESP_Debug(Esp, "Receive ACK timeout\n", Esp->TxAck);

		/* Check if any packets on acknowledgment list */
		ESP_Packet_t *Packet = Esp->TxPacketAckList;
		if (Packet)
		{
			/* Find end of awaiting acknowledgment list */
			while (Packet->Next)
				Packet = Packet->Next;

			/* Move list to head of transmit list */
			Packet->Next = Esp->TxPacketList;
			Esp->TxPacketList = Esp->TxPacketAckList;
			Esp->TxPacketAckList = NULL;
		}

		/* Restart receive acknowledgment timer */
		ESP_TxRestartRxAckTimer(Esp);
	}
	/* Find another packet to transmit */
	else if (Esp->TxPacketList)
	{
		/* Remove packet from list */
		Esp->TxPacket = Esp->TxPacketList;
		Esp->TxPacketList = Esp->TxPacket->Next;
		
		/* Check if sync packet to be sent */
		if (Esp->TxPacket->Type == ESP_PACKET_TYPE_SYNC)
		{
			Esp->TxPacketDataSize = ESP_SYNC_PKT_SIZE;
			Esp->TxPacketDataIndex = 0;
			ESP_Debug(Esp, "Pending Sync Packet %02x\n", Esp->TxPacket->Header[0]);
		}
		else if (Esp->TxPacket->Type == ESP_PACKET_TYPE_ACK)
		{
			Esp->TxPacketDataSize = ESP_ACK_PKT_SIZE;
			Esp->TxPacketDataIndex = 0;

			/* Update packet header */
			Esp->TxPacket->Header[0] &= ~ESP_PKT_ACK_MSK;
			Esp->TxPacket->Header[0] |= (Esp->TxAck << ESP_PKT_ACK_POS);

			ESP_Debug(Esp, "Pending Ack Packet %02x\n", Esp->TxPacket->Header[0]);			
		}
		else
		{
			/* Check if this packet needs a sequence number */
			if (!Esp->TxPacket->SeqNumberValid)
			{
				/* Return if we have reached limit of Tx window */
				const uint8_t Distance = (Esp->TxSeq - Esp->RxAck) & 0x07;
				if (Distance >= Esp->TxWindow)
					return;

				/* Set sequence number in packet */
				Esp->TxPacket->Header[0] &= ~ESP_PKT_SEQ_MSK;
				Esp->TxPacket->Header[0] |= (Esp->TxSeq << ESP_PKT_SEQ_POS);

				/* Mark sequence number as valid */
				Esp->TxPacket->SeqNumberValid = true;
				
				/* Advance sequence number */
				Esp->TxSeq = (Esp->TxSeq + 1) & 0x07;
			}
		
			/* Update packet header */
			Esp->TxPacket->Header[0] &= ~ESP_PKT_ACK_MSK;
			Esp->TxPacket->Header[0] |= (Esp->TxAck << ESP_PKT_ACK_POS);
						
			Esp->TxPacketDataSize = ESP_PKT_HEADER_SIZE + ((Esp->TxPacket->Header[1] & ESP_PKT_PAYLOAD_SIZE_MSK) >> ESP_PKT_PAYLOAD_SIZE_POS);
			Esp->TxPacketDataIndex = 0;
			ESP_Debug(Esp, "Pending payload packet, Channel %d, Seq %d, Ack %d, Size %d\n",
					  (Esp->TxPacket->Header[0] & ESP_PKT_CHANNEL_MSK) >> ESP_PKT_CHANNEL_POS,
					  (Esp->TxPacket->Header[0] & ESP_PKT_SEQ_MSK) >> ESP_PKT_SEQ_POS,
					  (Esp->TxPacket->Header[0] & ESP_PKT_ACK_MSK) >> ESP_PKT_ACK_POS,
					  (Esp->TxPacket->Header[1] & ESP_PKT_PAYLOAD_SIZE_MSK) >> ESP_PKT_PAYLOAD_SIZE_POS);
		}
	}
}

void ESP_TxPacket(ESP_t *Esp, ESP_Packet_t *TxPacket)
{
	/* Find end of transmit list */
	ESP_Packet_t *Packet = Esp->TxPacketList;
	if (Packet)
	{
		while (Packet->Next)
			Packet = Packet->Next;

		/* Add packet to end of transmit list */
		Packet->Next = TxPacket;
	}
	else
	{
		/* Nothing on transmit list */
		Esp->TxPacketList = TxPacket;
	}

	TxPacket->Next = NULL;
}

void ESP_TxInit(ESP_t *Esp)
{
	Esp->TxAck = 0x00;
	Esp->TxSeq = 0x00;
	Esp->TxWindow = 3;
	Esp->TxAckTimer = ESP_TIMER_IDLE;
	Esp->TxRetransmitPeriod = 1000;

	Esp->TxPacketAckList = Esp->TxPacketList = NULL;
	Esp->TxPacket = NULL;
}

void ESP_TxReset(ESP_t *Esp)
{
	ESP_Packet_t *Packet;
	
	Packet = Esp->TxPacketAckList;
	while (Packet)
	{
		ESP_Packet_t *PacketToFree = Packet;
		Packet = Packet->Next;
		ESP_DestroyPacket(PacketToFree);
	}
	
	Packet = Esp->TxPacketList;
	if (Packet)
	while (Packet)
	{
		ESP_Packet_t *PacketToFree = Packet;
		Packet = Packet->Next;
		ESP_DestroyPacket(PacketToFree);
	}
	
	if (Esp->TxPacket)
		ESP_DestroyPacket(Esp->TxPacket);
		
	ESP_TxInit(Esp);	
}

