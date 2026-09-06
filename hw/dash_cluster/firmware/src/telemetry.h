/*
 * telemetry.h
 *
 * Decoding the Toyotune boards' CAN telemetry into the signal store.
 *
 * Table driven, mirroring can_telemetry.c on the sending side: a frame is a
 * row, a signal within it is a row, and adding either should need no code.
 * The layout here must match toyotune.dbc, which is generated from the same
 * description the firmware uses - see tools/gen_dbc.py over there.
 *
 * WHICH BOARD ARE WE LISTENING TO?
 *
 * Both Toyotune boards publish the same signal names on disjoint identifier
 * blocks - 0x400 for CPU1, 0x420 for CPU2 - so a node has to choose. It
 * cannot merge them: RPM reaches CPU1's frame from CPU2's DMA block, so the
 * two carry near-identical values and interleaving them would just add jitter.
 */

#ifndef TELEMETRY_H_
#define TELEMETRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "signals.h"

/* Telemetry identifier blocks, from config.h on the board. */
#define TELEMETRY_BASE_CPU1	(0x400u)
#define TELEMETRY_BASE_CPU2	(0x420u)

/* Offsets within a block, from CanTelemetry_Frames[]. */
#define TELEMETRY_OFFSET_FAST		(0u)
#define TELEMETRY_OFFSET_MEDIUM1	(1u)
#define TELEMETRY_OFFSET_MEDIUM2	(2u)
#define TELEMETRY_OFFSET_SLOW		(3u)
#define TELEMETRY_OFFSET_RAW		(4u)
#define TELEMETRY_OFFSET_MEDIUM3	(5u)
#define TELEMETRY_OFFSET_INFO		(6u)


/* Select which board this node decodes. Call before Telemetry_Handle(). */
extern void Telemetry_Init(uint16_t TelemetryBase);

/* Feed one received frame. Returns true if it was one of ours and was
   decoded. Safe to call from the CAN callback - it only writes the store. */
extern bool Telemetry_Handle(uint16_t Id, const uint8_t *Data, uint8_t Length,
                             uint32_t NowMs);

/* True once an INFO frame has arrived carrying a protocol version this build
   does not understand. A caller should say so on screen rather than drawing
   gauges from bytes that may have moved. */
extern bool Telemetry_ProtocolMismatch(void);

/* The version last seen in an INFO frame, or 0 if none has arrived. */
extern uint8_t Telemetry_SeenProtocolVersion(void);

#endif /* TELEMETRY_H_ */
