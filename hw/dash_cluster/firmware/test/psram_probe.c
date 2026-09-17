/*
 * psram_probe.c
 *
 * Is PSRAM actually fitted to this board, and does it work?
 *
 * PLAN.md records the RP2350-Touch-AMOLED-1.75 as having a *reserved* PSRAM
 * pad - unpopulated - which is why the dash node keeps its back buffer at 8
 * bits and converts on the way out rather than holding a 16-bit framebuffer.
 * But Waveshare ship a complete PSRAM driver and a TLSF allocator in their C
 * examples, and a driver can exist for a footprint nobody filled. This
 * settles it.
 *
 * The answer matters: 466x466 at 16bpp is 424 KB, which does not fit in
 * 520 KB of SRAM alongside can2040, the faces and the application - the
 * paletted buffer is 212 KB and does. With PSRAM the 16-bit one would fit
 * easily, and paletting would become a preference rather than a necessity.
 *
 * WHY THIS DOES MORE THAN READ THE CHIP ID
 *
 * rp_setup_psram() reports a size if something answers the JEDEC Read-ID with
 * AP Memory's KGD byte (0x5D). That proves a chip is present and talking - it
 * does not prove the memory array works, that every address line is connected,
 * or that the timing the driver picked is right at this system clock. So this
 * also writes and reads back a pattern at several offsets spread across the
 * whole range, because a stuck or shorted address line passes a single-location
 * test perfectly.
 *
 * Build:  cmake --build build --target psram_probe
 * Flash:  picotool load -x build/psram_probe.uf2
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/structs/xip_ctrl.h"
#include "hardware/xip_cache.h"

#include "psram_tool.h"

/* RP2350 maps QSPI chip select 1 - the PSRAM select - here. */
#define PSRAM_LOCATION	0x11000000u

/* Offsets to exercise, as a fraction of whatever size is reported. Spread
   deliberately: testing only the first few bytes would pass even with every
   upper address line shorted together. */
static const unsigned Fractions[] = { 0, 1, 2, 3, 4, 5, 6, 7 };
#define FRACTION_COUNT	(sizeof(Fractions) / sizeof(Fractions[0]))
#define FRACTION_DIV	(8u)

/* A value that depends on the address, so a test cannot pass by reading back
   a neighbouring location or a stale cache line. */
static uint32_t Expected(uint32_t Offset)
{
	return (Offset * 2654435761u) ^ 0xA5A5A5A5u;
}


static void XipFlush(void)
{
	/* Push the writes out to the chip, then drop every cached copy so the
	   read-back has to come from the PSRAM itself. Without this a read can be
	   answered from cache and a broken write looks like a good one - which
	   would turn this test into an expensive way of checking the cache works.
	   RP2350's cache is driven through this API rather than the RP2040-style
	   xip_ctrl flush register, which does not exist here. */
	xip_cache_clean_all();
	xip_cache_invalidate_all();
}


static bool PatternTest(size_t Size)
{
	volatile uint32_t *Psram = (volatile uint32_t *)PSRAM_LOCATION;
	uint32_t Offsets[FRACTION_COUNT];
	bool Ok = true;
	unsigned i;

	/* The window is read-only until this is set - a write without it is
	   silently discarded, which reads as "PSRAM is broken". */
	xip_ctrl_hw->ctrl |= XIP_CTRL_WRITABLE_M1_BITS;

	for (i = 0; i < FRACTION_COUNT; i++)
	{
		/* Word-aligned, and kept inside the reported size. */
		uint32_t Off = (uint32_t)((Size / FRACTION_DIV) * Fractions[i]);

		if (Off > Size - 4u)
			Off = (uint32_t)(Size - 4u);
		Offsets[i] = Off & ~3u;
	}

	/* Write every location first, then read them all back. Interleaving write
	   and read would let a single working location and an aggressive cache
	   fake a pass. */
	for (i = 0; i < FRACTION_COUNT; i++)
		Psram[Offsets[i] / 4u] = Expected(Offsets[i]);

	XipFlush();

	for (i = 0; i < FRACTION_COUNT; i++)
	{
		uint32_t Got = Psram[Offsets[i] / 4u];
		uint32_t Want = Expected(Offsets[i]);

		printf("    +0x%08lX  wrote 0x%08lX  read 0x%08lX  %s\n",
		       (unsigned long)Offsets[i], (unsigned long)Want,
		       (unsigned long)Got, (Got == Want) ? "ok" : "MISMATCH");

		if (Got != Want)
			Ok = false;
	}

	return Ok;
}


int main(void)
{
	size_t Size;
	bool Pattern = false;

	stdio_init_all();

	/* The detection reconfigures the QMI and must not be interrupted by XIP
	   activity, so it is done once, early, before anything else runs. */
	Size = rp_setup_psram(RP2350_XIP_CSI_PIN);

	if (Size > 0)
		Pattern = PatternTest(Size);

	for (;;)
	{
		printf("\n=== PSRAM probe ===\n");

		if (Size == 0)
		{
			/* Nothing answered Read-ID with AP Memory's KGD byte. Either the
			   pad is empty - which is what PLAN.md assumes - or a fitted chip
			   is not responding at all. */
			printf("  no PSRAM detected on QSPI CS1 (pad %d)\n",
			       RP2350_XIP_CSI_PIN);
			printf("  the reserved pad is empty, or a fitted part is not answering\n");
			printf("  -> partial rendering stays a necessity\n");
		}
		else
		{
			printf("  detected %u bytes (%u MiB) on QSPI CS1\n",
			       (unsigned)Size, (unsigned)(Size / (1024u * 1024u)));
			printf("  address/data pattern test across the full range:\n");
			(void)PatternTest(Size);
			printf("  -> %s\n", Pattern
			       ? "PSRAM works; a full framebuffer becomes possible"
			       : "chip answers but the array does NOT read back - do not use it");
		}

		sleep_ms(3000);
	}
}
