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

/* Defaults to the MR2, because the image currently in image.c is a
   D151803-9651 DIAG16 build (the DIAG16 variant is the one to use - it is
   what implements the serial protocol diag.c speaks; see INSTALL.md section
   6).  Selecting TOYOTUNE_ECU_ST205 needs a matching ST205 image generated
   into image.c first - the frame layouts are shared, the ROM is not. */
#if !defined(TOYOTUNE_ECU_MR2) && !defined(TOYOTUNE_ECU_ST205)
#define TOYOTUNE_ECU_MR2
#endif

#if defined(TOYOTUNE_ECU_ST205)
/* The frame handling is family-independent, so the sniffing and parsing side
   is ready for the ST205.  The ROM image is not: image.c currently holds only
   a D151803-9651 DIAG16 (MR2) build, and this firmware writes that image into the
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


/* Sizes of the two DMA blocks on the inter-CPU USART, named by direction of
   travel.  These are absolute: a block is the same block whichever board is
   looking at it, so they do not change with TOYOTUNE_CPU1/CPU2.

     CPU1 -> CPU2   38 bytes   (MR2 CPU1 RAM 0x200..0x225, ST205 0x1FA..0x21F)
     CPU2 -> CPU1   34 bytes   (MR2 CPU1 RAM 0x226..0x247, ST205 0x220..0x241)

   Sizes come from each ROM's copy_dma_rx, which copies 16 bits at a time
   until the destination pointer reaches the end of the block.

   What DOES change with the CPU selection is which SERCOM carries which
   block.  The board sniffs the link pins of whichever Denso CPU it is
   plugged into, so on a CPU1 board SERCOM2 sees the CPU1 -> CPU2 block and
   SERCOM1 sees the reply; on a CPU2 board the two are reversed.  main.c
   binds them accordingly - see the SDL_Init calls.

   The sdl.c callbacks compare each captured length against these to confirm
   they caught a whole frame.  A short capture (sniffer started mid-burst, or
   a clipped burst) must be dropped rather than parsed, because parsing one
   would silently misalign every field after the truncation. */
#define TOYOTUNE_DMA_CPU1_TO_CPU2_SIZE   (38)
#define TOYOTUNE_DMA_CPU2_TO_CPU1_SIZE   (34)


#if defined(TOYOTUNE_CPU1)

/* CAN identifiers.  Each transmitting node needs identifiers of its own:
   two nodes sending the same ID cannot be separated by arbitration, so both
   run on into the data field and the first differing payload bit raises a
   bit error.  Keep CPU1 and CPU2 disjoint. */
/* Telemetry occupies a block of 11-bit standard identifiers, one block
   per board so the two never collide.  Kept well clear of 0x7DF/0x7E0/
   0x7E8, which OBD2 needs. */
#define TOYOTUNE_CAN_ID_TELEMETRY_BASE  (0x400)
#define TOYOTUNE_CAN_ID_FAST     (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 0)
#define TOYOTUNE_CAN_ID_MEDIUM1  (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 1)
#define TOYOTUNE_CAN_ID_MEDIUM2  (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 2)
#define TOYOTUNE_CAN_ID_SLOW     (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 3)
#define TOYOTUNE_CAN_ID_RAW      (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 4)
#define TOYOTUNE_CAN_ID_FILTER       (0x45a)

#else /* TOYOTUNE_CPU2 */

/* CPU2 identifiers, chosen to be disjoint from CPU1's above. */
/* Telemetry occupies a block of 11-bit standard identifiers, one block
   per board so the two never collide.  Kept well clear of 0x7DF/0x7E0/
   0x7E8, which OBD2 needs. */
#define TOYOTUNE_CAN_ID_TELEMETRY_BASE  (0x420)
#define TOYOTUNE_CAN_ID_FAST     (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 0)
#define TOYOTUNE_CAN_ID_MEDIUM1  (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 1)
#define TOYOTUNE_CAN_ID_MEDIUM2  (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 2)
#define TOYOTUNE_CAN_ID_SLOW     (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 3)
#define TOYOTUNE_CAN_ID_RAW      (TOYOTUNE_CAN_ID_TELEMETRY_BASE + 4)
#define TOYOTUNE_CAN_ID_FILTER       (0x45b)

/* CPU2 is not supported yet, but only one thing is actually missing: its ROM
   image.  image.c holds a CPU1 build, and image.c needs to select on
   TOYOTUNE_CPU1 / TOYOTUNE_CPU2 to serve the right one.

   The DMA side already works for either CPU.  Both CPUs see the same two
   blocks on the same link - only the direction each one transmits differs -
   so ECU_DmaData1_t/ECU_DmaData2_t serve both and main.c simply binds them
   to the opposite SERCOMs when built for CPU2.

   This is deliberately a hard error rather than a default, so that a CPU2
   build cannot silently come out parsing CPU1's frame layout. */
#error "TOYOTUNE_CPU2 is not implemented yet - see the notes above this line."

#endif

#endif /* CONFIG_H_ */
