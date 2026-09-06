/*
 * can_link.h
 *
 * can2040 - software CAN on a PIO block - and the node's own heartbeat.
 *
 * Compiled only when a can2040 checkout is present; see the CMakeLists. The
 * rest of the node builds and tests without it so decode and the page tables
 * can be worked on meanwhile.
 */

#ifndef CAN_LINK_H_
#define CAN_LINK_H_

#include <stdbool.h>
#include <stdint.h>

/* Heartbeat identifiers, one per node, clear of the telemetry blocks at
   0x400/0x420 and of OBD2's 0x7DF/0x7E0/0x7E8. */
#define CAN_LINK_HEARTBEAT_BASE		(0x440u)

#define CAN_LINK_BITRATE		(500000u)

/* GPIO25 and GPIO26 - the UART pins on the SH1.0 connector, the only pair on
   this board shared with no on-board peripheral. Both are far below 31, so
   they sit inside PIO's window on an RP2350A without further thought. */
#define CAN_LINK_GPIO_RX		(25u)
#define CAN_LINK_GPIO_TX		(26u)

/* The panel driver already uses a PIO block, so can2040 must be given a
   different one. Three exist; this leaves one spare. */
#define CAN_LINK_PIO_NUM		(1u)

extern void CanLink_Init(uint8_t NodeId);

/* Call from the main loop on core 0. Sends the heartbeat when due; receive is
   interrupt driven and needs no polling. */
extern void CanLink_Poll(uint32_t NowMs);

#endif /* CAN_LINK_H_ */
