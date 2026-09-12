# Waveshare panel drivers — vendored

The CO5300 AMOLED driver, its PIO-QSPI transport, the CST9217 touch driver and
the PSRAM helpers, taken from Waveshare's demo package for the
RP2350-Touch-AMOLED-1.75.

**Provenance**

```
https://files.waveshare.com/wiki/RP2350-Touch-AMOLED-1.75/RP2350-Touch-AMOLED-1.75-Demo.zip
  -> C/05_LVGL/lib/{AMOLED,QSPI_PIO,Config,Touch,PSRAM}
downloaded 2026-09-12, sources dated 2025-12-10, Waveshare Team
```

Licence: the sources carry an MIT-style grant ("Permission is hereby granted,
free of charge, to any person obtaining a copy…").

**Why these are vendored when can2040 and LVGL are not.** Those two are
maintained projects with public git history, so pinning a copy here would only
hide which revision is in use. This is a zip from a wiki with no upstream to
track — vendoring is the only way to have it at all, so it is here with its
provenance recorded instead.

## What each file is

| File | |
|---|---|
| `AMOLED_1in75.c/.h` | CO5300 init sequence, window addressing, brightness |
| `qspi_pio.c/.h`, `qspi.pio`, `qspi.pio.h` | The PIO program that drives the panel's QSPI |
| `DEV_Config.c/.h`, `Debug.h` | Pin definitions, I2C/SPI setup, clock |
| `CST9217.c/.h` | Capacitive touch over I2C |
| `psram_tool.c/.h`, `rp_pico_alloc.c/.h` | PSRAM detection and a TLSF allocator over it |
| `LVGL_example.c.reference` | **Reference only — do not build.** See the bug below |
| `lv_conf.h.reference` | Their LVGL config, for comparison |

## Three things found while reading this, before reusing any of it

### 1. The vendor LVGL example overflows its draw buffer

```c
#define LV_COLOR_DEPTH 16                                   /* lv_color_t is 2 bytes */
buf0 = (lv_color_t *)malloc(DISP_HOR_RES * DISP_VER_RES);   /* 217,156 BYTES */
lv_disp_draw_buf_init(&disp_buf, buf0, NULL,
                      DISP_HOR_RES * DISP_VER_RES);          /* 217,156 PIXELS */
```

The allocation is in bytes and the size handed to LVGL is in pixels. At 16-bit
colour those differ by a factor of two, so **LVGL believes it has 434,312
bytes in a 217,156-byte allocation** — a 212 KB heap overflow the moment it
redraws enough of the screen at once.

It evidently does not crash in the demo, presumably because partial refreshes
never touch the far half. That does not make it safe to copy. This is why the
dash firmware uses partial buffers (see `PLAN.md` §4.1a) rather than inheriting
this arrangement — and note that simply doubling the malloc would not fit
either: 424 KB of a 520 KB SRAM leaves nothing for LVGL's own 32 KB heap,
can2040 and the application.

### 2. The panel uses PIO0

`qspi_pio.c` sets `.pio = pio0`. can2040 therefore needs a different block;
`can_link.h` already selects PIO1 and `PIO1_IRQ_0`, which this confirms rather
than contradicts. One block is left spare.

### 3. The published pinout table is wrong in at least two places

Waveshare's pinout image, which the allocation in `PLAN.md` §4.1 was built
from, disagrees with their own code:

| Pin | Pinout image says | The code does |
|---|---|---|
| GPIO8 | `QSPI_SS2` | `DOF_INT1` — the IMU interrupt. `QMI8658.c` configures it as an input and reads it |
| GPIO24 | `GPS_RST` | `card_detect_gpio` — the microSD card-detect, in `hw_config.c` |

Both are *used* in code, which is stronger evidence than a marketing table.

**The consequence for the node-ID divider:** the image put `IMU_INT1` on
GPIO28, which is why §4.1 picked GPIO28 (ADC2) and noted the IMU interrupt
would be left disabled. If the interrupt is really on GPIO8, GPIO28 is
*cleaner* than assumed rather than worse — but the table it came from now has
two known errors, so **the allocation needs confirming against the schematic
rather than the image** before any carrier is laid out.

Nothing in the C examples references GPIO25–29 at all, which is at least
consistent with those five being the free ones.

## Answered on the board

**Is PSRAM actually populated? No.** `PLAN.md` said "reserved pad,
unpopulated", but a full PSRAM driver and a TLSF allocator ship in the C tree,
and a driver can exist for a footprint nobody filled. `psram_tool.c` detects it
at runtime, so this was answerable by flashing rather than by reading a
datasheet: `firmware/test/psram_probe.c` calls `rp_setup_psram()` against QSPI
chip select 1 (pad 47) and, if anything answers, pattern-tests eight offsets
spread across the reported range — because a stuck upper address line passes a
single-location test perfectly.

Flashed 2026-09-12:

```
=== PSRAM probe ===
  no PSRAM detected on QSPI CS1 (pad 47)
```

Nothing answers the JEDEC Read-ID with AP Memory's KGD byte (`0x5D`). The pad
is empty. So `rp_pico_alloc.c` and `psram_tool.c` are vendored here for
reference and for the probe, not for the node firmware, and **partial
rendering is a requirement rather than a preference** — there is no fallback
if it proves too slow, which raises the stakes on measuring it at M2.

Populating the footprint stays possible — an 8-pin SOIC APS6404L-class part on
the same JEDEC serial-memory footprint as the flash chip beside it — but that
is a hardware modification now, not something the board already offers. Its two
constraints if it is ever fitted: 109 MHz maximum clock, and an 8 µs ceiling on
chip-select assertion for refresh, so long DMA bursts to PSRAM have to be
broken up.
