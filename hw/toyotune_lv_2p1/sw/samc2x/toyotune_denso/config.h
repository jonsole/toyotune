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

/* --- Which car -----------------------------------------------------------

   Orthogonal to the CPU selection above.  Two 3S-GTE ECU families are
   supported, each a pair of Denso CPUs:

     TOYOTUNE_ECU_MR2    - JDM Gen 3 SW20, D151803-9651 (CPU1) / -9661 (CPU2)
     TOYOTUNE_ECU_ST205  - JDM ST205,      D151804-0461 (CPU1) / -0471 (CPU2)

   Both families use the same inter-CPU frame geometry - 38 bytes out, 34
   bytes back, with the same field slots - so ECU_DmaData1_t/ECU_DmaData2_t
   serve both and only the base addresses in their comments differ (MR2 TX
   starts at 0x200, ST205 at 0x1FA).  What this define really selects is the
   ROM image served from image.c.

   Define exactly one.  ------------------------------------------------- */

#if defined(TOYOTUNE_ECU_MR2) && defined(TOYOTUNE_ECU_ST205)
#error "Define exactly one of TOYOTUNE_ECU_MR2 / TOYOTUNE_ECU_ST205, not both."
#endif

/* Defaults to the MR2, because the image currently active in image.c is a
   D151803-9651 build.  Selecting TOYOTUNE_ECU_ST205 needs a matching ST205
   image added to image.c first - the frame layouts are shared, the ROM is
   not. */
#if !defined(TOYOTUNE_ECU_MR2) && !defined(TOYOTUNE_ECU_ST205)
#define TOYOTUNE_ECU_MR2
#endif

#if defined(TOYOTUNE_ECU_ST205)
/* The frame handling is family-independent, so the sniffing and parsing side
   is ready for the ST205.  The ROM image is not: image.c currently holds only
   a D151803-9651 (MR2) build, and this firmware writes that image into the
   SRAM the Denso CPU executes from.  Building ST205 today would therefore
   run MR2 code on an ST205 ECU.

   To finish ST205 support, add its image to image.c selected on this define,
   then delete this #error. */
#error "TOYOTUNE_ECU_ST205 needs its ROM image adding to image.c first - see above."
#endif


#if defined(TOYOTUNE_CPU1) && defined(TOYOTUNE_CPU2)
#error "Define exactly one of TOYOTUNE_CPU1 / TOYOTUNE_CPU2, not both."
#endif

/* Default used when the build system passes neither. The Atmel Studio
   project does not pass one, so it lands here; the CMake build passes
   -DTOYOTUNE_CPU1 or -DTOYOTUNE_CPU2 on the command line, which takes
   precedence over this. Edit this line to point the Studio build at the
   other CPU. */
#if !defined(TOYOTUNE_CPU1) && !defined(TOYOTUNE_CPU2)
#define TOYOTUNE_CPU1
#endif


#if defined(TOYOTUNE_CPU1)

/* Sizes in bytes of the two DMA blocks on the inter-CPU USART.  Named from
   the attached Denso CPU's point of view, matching the ROM's own dmatx_ /
   dmarx_ convention:

     TX - what this CPU sends to the other one    (CPU1 RAM 0x1FA..0x21F)
     RX - what this CPU receives back from it     (CPU1 RAM 0x220..0x241)

   The RX size is taken from copy_dma_rx in the D151804-0461 disassembly,
   which copies 16 bits at a time until the destination pointer reaches
   0x242: 0x242 - 0x220 = 0x22 = 34 bytes.

   The sdl.c callbacks compare each captured length against these to confirm
   they caught a whole frame.  A short capture (sniffer started mid-burst, or
   a clipped burst) must be dropped rather than parsed, because parsing one
   would silently misalign every field after the truncation. */
#define TOYOTUNE_DMA_TX_FRAME_SIZE   (38)
#define TOYOTUNE_DMA_RX_FRAME_SIZE   (34)

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
     - CPU2's DMA block sizes as TOYOTUNE_DMA_TX/RX_FRAME_SIZE here, plus
       for it in main.c.  CPU2's block is NOT ECU_DmaData1_t - that struct
       describes what CPU1 sends - so it needs its own struct and its own
       repacking into the telemetry frames above.

   This is deliberately a hard error rather than a default, so that a CPU2
   build cannot silently come out parsing CPU1's frame layout. */
#error "TOYOTUNE_CPU2 is not implemented yet - see the notes above this line."

#endif

#endif /* CONFIG_H_ */
