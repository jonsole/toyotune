/*
 * config.h
 *
 * Build-time selection of which Denso CPU of the ECU this board is attached to.
 */


#ifndef CONFIG_H_
#define CONFIG_H_

/* ---------------------------------------------------------------------------
   The ECU contains two Denso CPUs, and each gets its own Toyotune board
   (SAMC21 + shared SRAM + CPLD), with both boards on the same CAN bus.  One
   firmware source serves both; this define says which CPU this binary is for.

       TOYOTUNE_CPU1 - D151804-0461 (ST205)  /  D151803-9651 (SW20)
       TOYOTUNE_CPU2 - D151804-0471 (ST205)  /  D151803-9661 (SW20)

   Everything that differs between the two boards must be derived from this
   define and from nothing else:

     - the ROM image served to the Denso CPU out of image.c
     - the inter-CPU DMA frame layout parsed by the sdl.c callback, since the
       two CPUs put different payloads on the link
     - the CAN identifiers, so that the two boards do not collide on the bus

   Define exactly one.
   --------------------------------------------------------------------------- */

#define TOYOTUNE_CPU1

#if defined(TOYOTUNE_CPU1) && defined(TOYOTUNE_CPU2)
#error "Define exactly one of TOYOTUNE_CPU1 / TOYOTUNE_CPU2, not both."
#endif

#if !defined(TOYOTUNE_CPU1) && !defined(TOYOTUNE_CPU2)
#error "Define one of TOYOTUNE_CPU1 / TOYOTUNE_CPU2."
#endif


#if defined(TOYOTUNE_CPU1)

/* Size in bytes of one complete DMA block from this CPU on the inter-CPU
   USART.  The sdl.c callback compares the captured length against this to
   confirm it caught a whole frame: a short capture (sniffer started
   mid-burst, or a clipped burst) must be dropped rather than parsed, because
   parsing one would silently misalign every field after the truncation. */
#define TOYOTUNE_DMA_FRAME_SIZE      (38)

/* CAN identifiers.  Each transmitting node needs identifiers of its own:
   two nodes sending the same ID cannot be separated by arbitration, so both
   run on into the data field and the first differing payload bit raises a
   bit error.  Keep CPU1 and CPU2 disjoint. */
#define TOYOTUNE_CAN_ID_TELEMETRY_1  (0x1001)
#define TOYOTUNE_CAN_ID_TELEMETRY_2  (0x1002)
#define TOYOTUNE_CAN_ID_FILTER       (0x45a)

#else /* TOYOTUNE_CPU2 */

/* CPU2 identifiers, chosen to be disjoint from CPU1's above. */
#define TOYOTUNE_CAN_ID_TELEMETRY_1  (0x1011)
#define TOYOTUNE_CAN_ID_TELEMETRY_2  (0x1012)
#define TOYOTUNE_CAN_ID_FILTER       (0x45b)

/* CPU2 is not supported yet.  To finish it, supply:

     - CPU2's ROM image in image.c.  The image active there today is CPU1's,
       so image.c needs to select on TOYOTUNE_CPU1 / TOYOTUNE_CPU2 too.
     - CPU2's DMA block size as TOYOTUNE_DMA_FRAME_SIZE here, plus a layout
       for it in main.c.  CPU2's block is NOT ECU_DmaData1_t - that struct
       describes what CPU1 sends - so it needs its own struct and its own
       repacking into the telemetry frames above.

   This is deliberately a hard error rather than a default, so that a CPU2
   build cannot silently come out parsing CPU1's frame layout. */
#error "TOYOTUNE_CPU2 is not implemented yet - see the notes above this line."

#endif

#endif /* CONFIG_H_ */
