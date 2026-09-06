/*
 * can_link.c
 *
 * can2040 setup, the receive callback, and the node heartbeat.
 *
 * THIS NODE MUST ACKNOWLEDGE.
 *
 * It is tempting to think of a gauge as a listener that need not transmit,
 * and to configure it accordingly. That would be wrong here: CAN
 * acknowledgement is a transmit action - a dominant bit driven in the ACK
 * slot - and with one Toyotune board plus this node, this node is the only
 * other station on the bus. A listen-only gauge would leave the board
 * error-passive with nothing acknowledging its frames. can2040 does full
 * transmit as well as receive, which is why it suits.
 *
 * TIMING
 *
 * can2040 decodes the bus in its PIO interrupt and is sensitive to interrupt
 * latency; its documentation asks that its code, and anything at higher
 * priority, live in SRAM rather than XIP flash, because a cache miss inside
 * the CAN interrupt corrupts a bit. Hence the section attributes below.
 *
 * The remaining risk is the panel's DMA competing for bus bandwidth with the
 * core servicing this interrupt. That is measured at M4 and, if it bites, the
 * levers are in PLAN.md section 4.2a - bank placement first, then capping the
 * display burst, and BUSCTRL priority last, since it is a strict priority and
 * can starve the renderer instead.
 */

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/irq.h"
#include "hardware/regs/intctrl.h"
#include "hardware/structs/pio.h"

#include "can2040.h"

#include "can_link.h"
#include "signal_store.h"
#include "telemetry.h"

static struct can2040 CanBus;
static uint8_t HeartbeatNodeId;
static uint32_t NextHeartbeatMs;
static uint32_t FramesDecoded;
static uint32_t RxErrors;

#define HEARTBEAT_PERIOD_MS	(1000u)


/***************************************************************************************/
/* Called from the PIO interrupt. Decode only - no drawing, no blocking, and
   nothing that could take a lock the renderer might hold. Writing the signal
   store is safe here precisely because it is a seqlock: the writer never
   waits for a reader. */
static void CanLink_Callback(struct can2040 *Bus, uint32_t NotifyType,
                             struct can2040_msg *Msg)
{
	(void)Bus;

	if (NotifyType == CAN2040_NOTIFY_ERROR)
	{
		RxErrors++;
		return;
	}

	if (NotifyType != CAN2040_NOTIFY_RX || Msg == NULL)
		return;

	/* Extended frames are not ours: all telemetry uses 11-bit identifiers. */
	if (Msg->id & CAN2040_ID_EFF)
		return;

	if (Telemetry_Handle((uint16_t)(Msg->id & 0x7FFu), Msg->data, Msg->dlc,
	                     to_ms_since_boot(get_absolute_time())))
		FramesDecoded++;
}


/***************************************************************************************/
static void __not_in_flash_func(CanLink_PioIrqHandler)(void)
{
	can2040_pio_irq_handler(&CanBus);
}


/***************************************************************************************/
void CanLink_Init(uint8_t NodeId)
{
	uint32_t SysClockHz = clock_get_hz(clk_sys);

	HeartbeatNodeId = NodeId;
	NextHeartbeatMs = 0;
	FramesDecoded = 0;
	RxErrors = 0;

	can2040_setup(&CanBus, CAN_LINK_PIO_NUM);
	can2040_callback_config(&CanBus, CanLink_Callback);

	irq_set_exclusive_handler(PIO1_IRQ_0, CanLink_PioIrqHandler);
	irq_set_priority(PIO1_IRQ_0, 0);	/* highest - see the timing note above */
	irq_set_enabled(PIO1_IRQ_0, true);

	can2040_start(&CanBus, SysClockHz, CAN_LINK_BITRATE,
	              CAN_LINK_GPIO_RX, CAN_LINK_GPIO_TX);
}


/***************************************************************************************/
/* The heartbeat distinguishes "gauge 2 is dead" from "gauge 2's panel is
   dead" - without it, a node that boots but never renders looks identical to
   one that never powered up. It also carries the current page, which is what
   would make coordinated paging possible later without a new frame; include
   the byte now even though paging is independent, because adding it later
   would be a protocol change. */
void CanLink_Poll(uint32_t NowMs)
{
	struct can2040_msg Msg;
	uint32_t UptimeS;

	if ((int32_t)(NowMs - NextHeartbeatMs) < 0)
		return;

	NextHeartbeatMs = NowMs + HEARTBEAT_PERIOD_MS;

	UptimeS = NowMs / 1000u;

	memset(&Msg, 0, sizeof(Msg));
	Msg.id = CAN_LINK_HEARTBEAT_BASE + HeartbeatNodeId;
	Msg.dlc = 8;
	Msg.data[0] = HeartbeatNodeId;
	Msg.data[1] = 0;	/* current page - filled in once paging is wired up */
	Msg.data[2] = (uint8_t)(UptimeS >> 8);
	Msg.data[3] = (uint8_t)(UptimeS & 0xFFu);
	Msg.data[4] = (uint8_t)(FramesDecoded >> 8);
	Msg.data[5] = (uint8_t)(FramesDecoded & 0xFFu);
	Msg.data[6] = (uint8_t)((RxErrors > 0xFFu) ? 0xFFu : RxErrors);
	Msg.data[7] = SignalStore_LinkAlive(NowMs) ? 1u : 0u;

	/* A failed transmit is not retried and not counted as fatal: the bus may
	   simply be busy, and a gauge must never stall waiting to talk about
	   itself. */
	(void)can2040_transmit(&CanBus, &Msg);
}
