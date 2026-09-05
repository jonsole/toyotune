---
name: samc21-build
description: Build, verify and flash the SAMC21 firmware (hw/toyotune_lv_2p1/sw/samc2x/toyotune_denso). Use when asked to build or rebuild the board firmware, when a cmake/ninja/arm-none-eabi build of it fails, when image.c needs regenerating from a ROM source, or when flashing the .elf to the board over SWD.
---

# Building the SAMC21 firmware

Everything here is in `hw/toyotune_lv_2p1/sw/samc2x/toyotune_denso`. **Run the
commands from that directory.**

`INSTALL.md` in that directory is the authoritative guide and goes deeper than
this skill on packs, VS Code and the ROM image. This skill is the fast path plus
the things that actually go wrong on this machine.

## 0. The gotcha that stops most builds

**`cmake` is not on PATH.** `ninja` and `arm-none-eabi-gcc` are (both from
Chocolatey, in `C:/ProgramData/chocolatey/bin`), so a build looks like it should
work and then dies at the first command. There are two CMakes installed:

| Path | Version |
|---|---|
| `C:/Program Files/CMake/bin/cmake.exe` | 4.4.3 — **prefer this** |
| `C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe` | 3.31.6 |

Both work — the project asks for 3.21+, so CMake 4.x does not trip the removed
`cmake_minimum_required` compatibility. Set it once per shell:

```
CM="/c/Program Files/CMake/bin/cmake.exe"
```

Shell state does not persist between Bash tool calls, so put that assignment in
the *same* call as the command that uses it.

## 1. Build

```
cd hw/toyotune_lv_2p1/sw/samc2x/toyotune_denso
CM="/c/Program Files/CMake/bin/cmake.exe"
"$CM" --preset mr2-cpu1-debug          # configure (only needed once per preset)
"$CM" --build --preset mr2-cpu1-debug  # build
```

`build/<preset>/` usually already exists and is configured, in which case just
the second line is enough — and `--build build/<preset>` works too if you prefer
a path to a preset name. A no-op build prints `ninja: no work to do.`

`build/` is gitignored, so deleting a build directory is always safe.

## 2. Presets

| Preset | Builds |
|---|---|
| `mr2-cpu1-debug` | MR2 CPU1 (D151803-9651 DIAG16), `-O0 -g3` |
| `mr2-cpu1-release` | MR2 CPU1, `-Os` |
| `mr2-cpu2-debug` | **expected to fail** — deliberate `#error` in `config.h` |
| `st205-cpu1-debug` | **expected to fail** — deliberate `#error` in `config.h` |

The last two stop at `#error "TOYOTUNE_CPU2 is not implemented yet"` /
the ST205 equivalent. That is **correct behaviour, not a broken build** —
failing beats silently flashing CPU1 code into a CPU2 board, or MR2 code into an
ST205 ECU. Do not "fix" it by deleting the `#error`; it needs the matching ROM
image in `image.c` first. If a session reports "the build fails", check which
preset they used before investigating anything else.

The two axes are independent cache variables, so any combination is reachable
without touching presets: `-DTOYOTUNE_CPU=1|2 -DTOYOTUNE_ECU=MR2|ST205`.

## 3. Output

In `build/<preset>/`: `toyotune_denso.elf` (what you flash), plus `.bin`, `.hex`
and `.lss` (disassembly listing). The link step prints the size table — text,
data, bss.

## 4. The ROM image — `image.c` is generated

`image.c` holds the 32K Denso ROM image the firmware writes into the shared SRAM
before releasing the Denso MCU from reset. **Never hand-edit it.** It is built
from `roms/3S-GTE/D151803-9651/toyotune/D151803-9651_DIAG16_32K.ASM` — the
*DIAG16* variant, not the stock ROM, because only DIAG16 implements the
diagnostic protocol `diag.c` talks to.

After changing that `.ASM`, regenerate before building:

```
python tools/build_image.py ../../../../../roms/3S-GTE/D151803-9651/toyotune/D151803-9651_DIAG16_32K.ASM -o image.c
```

To check the committed `image.c` is in step with the ROM source without writing
anything (note `--verify` takes the file as an argument):

```
python tools/build_image.py <asm-path> --verify image.c
```

It prints `OK: image.c matches <asm>` or exits 1 on drift. CMake also exposes
these as the `rom-image` and `verify-rom-image` targets.

The script assembles with `roms/d8x_assembler/asm_d8x.py -p 5F`, pads to 32K and
patches the `0xAA55` checksum, reading the checksummed range out of the image
rather than assuming it — the DIAG16 images sum from `8000`, the 16K stock ROMs
from `C000`. **A changed diagnostic block changes the checksum**, so skipping
this step gives a ROM that fails its own self test.

## 5. When it fails

| Message | Cause |
|---|---|
| `cmake: command not found` | §0 — not on PATH |
| `SAMC21_DFP_DIR does not exist: <path>` | device packs missing or `PACKS_DIR` wrong — see §6 |
| `#error "TOYOTUNE_CPU2 is not implemented yet"` | wrong preset, working as designed — §2 |
| `#error` naming ST205 | same, §2 |
| `fatal error: sam.h: No such file` | packs found but wrong layout — `PACKS_DIR` needs `SAMC21_DFP/samc21/include` and `CMSIS/CMSIS/Core/Include` beneath it |
| a new `.c` builds but does not link | `CMakeLists.txt` lists sources **explicitly**, deliberately — a glob would drag in the AVR-era files. Add the file to the list. |
| ROM self test fails on the bench | `image.c` not regenerated after editing the DIAG16 `.ASM` — §4 |

`mode.c`, `toyotune_avr.c`, `sram.c` and the whole `esp*` set are in the
directory but **not in the build**. That is intentional; do not add them.

## 6. Device packs

`PACKS_DIR` defaults to `hw/toyotune_lv_2p1/sw/packs`, which **exists on this
machine but is gitignored** — so it is present here and absent on a fresh clone.
It needs `SAMC21_DFP/samc21/include/sam.h` and
`CMSIS/CMSIS/Core/Include/core_cm0plus.h`.

If they are missing, `INSTALL.md` §2 has the sourcing detail. The short version:
the SAMC21 DFP must be **pre-3.0** (3.x rewrote the headers to MPLAB Harmony
style and yields ~300 errors); get `Atmel.SAMC21_DFP.1.2.176.atpack` and
`ARM.CMSIS.5.4.0.atpack` from `packs.download.atmel.com` — the newer
`packs.download.microchip.com` only goes back to DFP 2.0.5. `.atpack` files are
ordinary ZIPs.

**If your paths differ, do not edit `CMakePresets.json`** — it is shared. Create
`CMakeUserPresets.json` beside it (already gitignored) inheriting from the
preset you want and overriding `PACKS_DIR` / `ARM_TOOLCHAIN_DIR`. `INSTALL.md`
§"Machine-specific paths" has a template — note its example inherits from
`cpu1-debug`, which is a stale name; the presets are `mr2-cpu1-debug` etc.

## 7. Flashing

Over SWD with the Atmel-ICE. Via the pyocd MCP tools, or directly:

```
hw/toyotune_lv_2p1/sw/python/.venv/Scripts/pyocd.exe flash \
    -t atsamc21j18a -u J41800034284 build/mr2-cpu1-debug/toyotune_denso.elf
```

Two things that bite:

- **The first flash attempt often fails** with `flash erase sector failure` or
  `SWD/JTAG communication failure (Unexpected ACK)`. Simply retrying usually
  works; halting the target first helps. If pyocd hangs even on `list`, the
  Atmel-ICE has wedged and needs a USB replug — ask the user, do not keep
  retrying.
- **`connect_mode=under-reset` does not work on this board** (SWD no-ACK). Use
  the default connect mode.

Reset the target running afterwards, or the board sits halted and the ECU never
comes out of reset.

## 8. Keeping the two build systems in step

There is also an Atmel Studio 7 project (`toyotune_denso.atsln`/`.cproj`)
covering the same sources. If you add or remove a source file, update **both**
it and `CMakeLists.txt`, or the Studio build silently diverges.
