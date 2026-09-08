---
name: rp2350-build
description: Build, test and flash the RP2350 dash node firmware (hw/dash_cluster/firmware). Use when asked to build or rebuild the dash/gauge firmware, when a cmake/ninja build of it fails, when running its host tests, or when flashing a dash node over USB.
---

# Building the RP2350 dash node firmware

Everything here is in `hw/dash_cluster/firmware`. **Run the commands from that
directory.** `README.md` there covers the code layout; this skill is the build
itself and the things that actually go wrong.

The design decisions - why the cores split the way they do, why identity is a
resistor divider, why LVGL 8 - are in `hw/dash_cluster/PLAN.md`. Do not
re-litigate them from the code alone.

## 0. The gotcha that stops most builds

**Nothing is on PATH.** The Pico VS Code extension installs the SDK, the ARM
toolchain, CMake and Ninja under `~/.pico-sdk`, and a plain shell finds none of
them. `arm-none-eabi-gcc` on PATH is the *Chocolatey* 10.3 build used for the
SAMC21 - not the 14.2 one this project wants.

Set all four in the **same** Bash call as the command that uses them; shell
state does not persist between calls.

```
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/14_2_Rel1
CM=~/.pico-sdk/cmake/v3.31.5/bin/cmake.exe
NINJA=~/.pico-sdk/ninja/v1.12.1/ninja.exe
```

Versions move when the extension updates - `ls ~/.pico-sdk/sdk` if a path is
wrong rather than assuming the numbers above.

## 1. Build

```
cd hw/dash_cluster/firmware
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/14_2_Rel1
~/.pico-sdk/cmake/v3.31.5/bin/cmake.exe -B build -G Ninja \
    -DCMAKE_MAKE_PROGRAM=~/.pico-sdk/ninja/v1.12.1/ninja.exe
~/.pico-sdk/cmake/v3.31.5/bin/cmake.exe --build build
```

Configure is only needed once. Output is `build/dash_node.uf2` (what you
flash), plus `.elf` and `.bin`. `build/` is gitignored, so deleting it is
always safe.

A clean build is around **37 KB text** with neither optional dependency. Ninja
prints `ninja: no work to do.` for a no-op.

## 2. Host tests

The decode, signal store, node identity, page selection and UI model have no
hardware dependency **on purpose**, so they can be tested without a board:

```
cd hw/dash_cluster/firmware
python test/run_tests.py
```

Around 400 checks, and it finds MSVC through vswhere by itself. These are the
places a mistake is silent - a wrong byte offset gives a gauge that reads
plausibly and wrongly - so **run them after touching anything under `src/`
except `ui_lvgl.c`**, which is the one file they cannot cover.

## 3. The optional dependencies

can2040 and LVGL are **not vendored**. The build works without either and says
so; it does not silently produce firmware with no CAN.

```
git clone https://github.com/KevinOConnor/can2040        external/can2040
git clone -b release/v8.3 https://github.com/lvgl/lvgl   external/lvgl
```

`external/` is beside the repo root - or pass `-DCAN2040_PATH=` /
`-DLVGL_PATH=`. Watch the configure output: two `CMake Warning` blocks mean
you built a node that cannot receive and cannot draw.

**LVGL 8.x, not 9.** Waveshare's CO5300 panel driver and CST9217 touch glue are
written against 8.1. Porting both is deliberate work, not a free upgrade.

## 4. Flashing

The board mounts as a mass-storage device: hold BOOTSEL while plugging it in
and copy `build/dash_node.uf2` onto the drive that appears. That needs no
probe and is the normal path.

`picotool` is also installed and can reboot a running board into the
bootloader, so BOOTSEL only has to be held once:

```
~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load -x build/dash_node.uf2
```

**Do not reach for pyOCD here.** The two probes on this bench - the Atmel-ICE
`J41800034284` and the EDBG `ATML2419050200001722` - are the Toyotune board and
the stimulator, both ATSAMC21J18A. Pointing either at an RP2350, or flashing an
RP2350 image with a SAMC21 target type, is the same class of mistake the
stimulator skill warns about.

## 5. When it fails

| Message | Cause |
|---|---|
| `cmake: command not found` | §0 - nothing is on PATH |
| `No CMAKE_C_COMPILER could be found` while building **picotool** | The SDK is building picotool from source, which needs a **host** compiler. `CMakeLists.txt` should find the prebuilt one under `~/.pico-sdk/picotool/*/picotool`; if the extension moved it, pass `-Dpicotool_DIR=<dir containing picotoolConfig.cmake>` |
| `can2040 not found` / `LVGL not found` | §3, and expected on a fresh clone |
| Undefined `lv_*` symbols | LVGL was found at configure time but is a v9 checkout - the binding is written against 8.x |
| Host tests fail to compile with `__asm__` errors | Something added GCC inline asm to a file the host tests build. Guard it on `_MSC_VER`, as `signal_store.c` does for its memory barrier |

## 6. Two things about the code that are easy to undo by accident

- **`ui_model.c` must not gain an LVGL dependency.** The split exists so the
  decisions a gauge makes - staleness, off-scale needles, what shows before
  any data arrives - are testable on a host. Put drawing in `ui_lvgl.c`.
- **can2040 wants its code in SRAM**, not XIP flash: a cache miss inside the
  CAN interrupt corrupts a bit. `can_link.c` marks its handler
  `__not_in_flash_func`; keep that when editing it, and see PLAN.md §4.2a
  before adding anything else to that interrupt.

## 7. State of play

The board had not arrived when this was written, so what is missing is
missing on purpose:

- The **CO5300 flush** and **CST9217 touch read** callbacks - the only
  genuinely hardware-specific code. Everything above them exists.
- **Flash-backed page persistence.** `Pages_Init()` already takes a restored
  index; nothing stores one.
- The heartbeat's **page byte**, reserved and sent as zero.

Core 1 currently prints what it *would* draw over USB serial, which is enough
to exercise the store and the page tables before any glass is attached.
