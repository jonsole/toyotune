---
name: rp2350-build
description: Build, test and flash the RP2350 dash node firmware (hw/dash_cluster/firmware). Use when asked to build or rebuild the dash/gauge firmware, when a cmake/ninja build of it fails, when running its host tests, or when flashing a dash node over USB.
---

# Building the RP2350 dash node firmware

Everything here is in `hw/dash_cluster/firmware`. **Run the commands from that
directory.** `README.md` there covers the code layout; this skill is the build
itself and the things that actually go wrong.

The design decisions - why the cores split the way they do, why identity is a
resistor divider - are in `hw/dash_cluster/PLAN.md`. Do not re-litigate them
from the code alone. One exception: PLAN.md predates the renderer change and
still describes a build around LVGL. **`hw/dash_cluster/RENDERER_PLAN.md` is
current** - LVGL is no longer linked into the firmware at all, and
`firmware/README.md` describes what replaced it.

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

A full build with the panel and can2040 in is around **42 KB text**, and the
whole image about 440 KB - almost all of the rest being the two pre-rendered,
paletted dials in `.rodata`, 195 KB each. It was 366 KB of text and ~1,265 KB of image with LVGL, so
a build that has somehow found LVGL again is obvious from the size alone. Ninja
prints `ninja: no work to do.` for a no-op.

The build should be **clean**: `DASH_SOURCES` are compiled `-Wall -Wextra
-Wconversion` on purpose, because this code moves between widths constantly and
a silent narrowing gives a plausible wrong gauge. A new warning is a finding,
not noise. The vendor and can2040 sources are deliberately excluded from that
set.

## 2. Host tests

The decode, signal store, node identity, page selection and UI model have no
hardware dependency **on purpose**, so they can be tested without a board:

```
cd hw/dash_cluster/firmware
python test/run_tests.py
```

372 checks, and it finds MSVC through vswhere by itself. These are the places a
mistake is silent - a wrong byte offset gives a gauge that reads plausibly and
wrongly - so **run them after touching anything under `src/`**. What they
cannot cover is the display path: `panel.c` needs the vendor driver and a
panel. The faces' palettising is checked by `build_faces.py` itself, which
refuses to write a face that does not decode back exactly.

## 3. Dependencies - one, and it is optional

**can2040 is the only external dependency the firmware has.** It is not
vendored. The build works without it and says so; it does not silently produce
firmware with no CAN.

```
git clone https://github.com/KevinOConnor/can2040        external/can2040
```

`external/` is beside the repo root - or pass `-DCAN2040_PATH=`. Watch the
configure output: a `CMake Warning` there means you built a node that cannot
receive.

The display has its own switch, `-DDASH_HAVE_PANEL=OFF`, which drops `panel.c`,
`ui_draw.c` and the faces and has core 1 report over USB serial instead of
drawing. Useful for working on the store and the page tables with no glass
attached. Default is ON.

**LVGL is not part of this build.** The dials are pre-rendered pictures and
what is drawn live is a needle and a number, so the toolkit was costing 5.5 ms
of a 16.8 ms scan, 260 KB of flash and 234 KB of SRAM to do very little - see
`RENDERER_PLAN.md`. `src/ui_draw.c` replaced it: an 8-bit paletted back buffer
the size of the screen, converted to RGB565 by `panel.c` a chunk of lines at a
time on its way out. **An LVGL checkout is not needed to build the firmware and
`-DLVGL_PATH=` no longer exists.**

### 3a. LVGL, where it still lives

LVGL is still the right tool for *designing* a dial, where a tick ring costing
two thirds of a frame does not matter because there are no frames. So it moved
to the PC, and regenerating the faces is the only thing that needs it:

```
git clone -b v9.5.0 https://github.com/lvgl/lvgl         external/lvgl
python tools/face_render/build_faces.py
```

That builds `tools/face_render` with MSVC and rewrites `src/dash_faces.c`.
**Rerun it after changing anything that decides how a dial looks** - tick
labels or placement in `pages.c`, `UiGauge_CreateScale()` in
`tools/face_render/ui_gauge.c`, the geometry in `src/ui_gauge.h`, or either
`lv_conf.h`. The firmware refuses a face whose size no longer matches its
element, but it cannot notice changed tick labels: a stale face just shows the
old numbers.

`firmware/lv_conf.h` survives for that tool alone, wrapped by
`tools/face_render/lv_conf.h`, which overrides only what cannot apply on a PC.
Deliberately the same file underneath, not a copy: the dial is a faithful
picture of what the board draws only if the fonts, colour depth and drawing
options match exactly. **`lv_conf.h` options must be v9 names** - v9 renamed
many of them, and a name it does not know is silently ignored rather than
rejected. To check one, grep for it in `external/lvgl/src/lv_conf_internal.h`;
if it is not there, v9 ignores it.

## 4. Flashing

The board mounts as a mass-storage device: hold BOOTSEL while plugging it in
and copy `build/dash_node.uf2` onto the drive that appears. That needs no
probe and is the normal path.

`picotool` can also reboot a **running** board into the bootloader, so BOOTSEL
never has to be held at all after the first time - confirmed on the board:

```
~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe reboot -f -u   # into BOOTSEL
~/.pico-sdk/picotool/2.2.0-a4/picotool/picotool.exe load -x build/dash_node.uf2
```

`picotool info -a` on a board in BOOTSEL reports the chip revision, package
and the resident binary's SDK version and build date - worth reading before
overwriting anything. **Save what is already there first:**

```
picotool.exe save vendor_demo_backup.uf2
```

The board ships with a Waveshare demo that is the only known-good proof the
panel works. Overwrite it without a copy and a blank screen becomes ambiguous
between "my code is wrong" and "the panel needs something I have not done".

**Do not reach for pyOCD here.** The two probes on this bench - the Atmel-ICE
`J41800034284` and the EDBG `ATML2419050200001722` - are the Toyotune board and
the stimulator, both ATSAMC21J18A. Pointing either at an RP2350, or flashing an
RP2350 image with a SAMC21 target type, is the same class of mistake the
stimulator skill warns about.

## 4a. Reading the USB serial output - assert DTR

**A board that looks dead on serial usually is not.** `pico_stdio_usb`
discards everything it prints unless `tud_cdc_connected()` is true, and that
is only true once the host raises **DTR**. Several clients leave it low by
default - including PowerShell's `System.IO.Ports.SerialPort`, where
`DtrEnable` is `false` - so the port opens, reads cleanly, and returns
nothing.

```powershell
$p = New-Object System.IO.Ports.SerialPort 'COM5',115200,'None',8,'one'
$p.DtrEnable = $true      # without this the firmware's output is thrown away
$p.RtsEnable = $true
$p.Open()
```

Two related things about CDC that are not bugs:

- **Anything printed before a host attaches is gone.** The boot banner is
  therefore invisible to anyone who connects afterwards, which on a board that
  powers up with the ignition is everyone. That is why the node identity is
  repeated in the periodic status line rather than only announced at boot.
- **`picotool` needs the port free.** If a terminal holds it, `picotool info`
  still works but reading the port yourself fails with access denied - the
  same single-owner problem the CANable has.

The board's COM number also moves between the vendor demo and our firmware;
find it rather than assuming:

```powershell
Get-CimInstance Win32_PnPEntity |
  Where-Object { $_.PNPDeviceID -match "VID_2E8A" -and $_.Name -match "COM" } |
  Select-Object Name
```

## 5. When it fails

| Message | Cause |
|---|---|
| `cmake: command not found` | §0 - nothing is on PATH |
| `No CMAKE_C_COMPILER could be found` while building **picotool** | The SDK is building picotool from source, which needs a **host** compiler. `CMakeLists.txt` should find the prebuilt one under `~/.pico-sdk/picotool/*/picotool`; if the extension moved it, pass `-Dpicotool_DIR=<dir containing picotoolConfig.cmake>` |
| `can2040 not found` | §3, and expected on a fresh clone |
| Undefined `lv_*` symbols in the **firmware** link | Something under `src/` has reached for LVGL again. Nothing there may: §3, and put drawing in `ui_draw.c` |
| `build_faces.py` cannot find LVGL, or `lvgl.h` is missing | §3a - the face renderer needs the checkout even though the firmware does not |
| `region RAM overflowed` | The 212 KB back buffer leaves less headroom than before. Check what grew with `arm-none-eabi-size -A build/dash_node.elf` before making anything smaller by guesswork |
| A `-Wconversion` warning in `src/` | Not noise - §1. Fix the narrowing rather than silencing it |
| Host tests fail to compile with `__asm__` errors | Something added GCC inline asm to a file the host tests build. Guard it on `_MSC_VER`, as `signal_store.c` does for its memory barrier |

## 6. Two things about the code that are easy to undo by accident

- **`ui_model.c` must not gain a display dependency.** The split exists so the
  decisions a gauge makes - staleness, off-scale needles, what shows before
  any data arrives - are testable on a host. Put drawing in `ui_draw.c`.
- **can2040 wants its code in SRAM**, not XIP flash: a cache miss inside the
  CAN interrupt corrupts a bit. `can_link.c` marks its handler
  `__not_in_flash_func`; keep that when editing it, and see PLAN.md §4.2a
  before adding anything else to that interrupt.

## 7. State of play

The board arrived 2026-09-12 and the firmware ran on it, drawing on the glass
through LVGL: the CO5300 flush and the CST9217 touch read are both implemented
in `src/panel.c`, and the needle gauges animated without tearing once each
flush was synced to the panel's TE pulse and sent as one burst. The node
identity falls back safely with no divider fitted. Confirmed as
**RP2350 rev A2, QFN60** - i.e. RP2350A, GPIO0..29, so every pin is inside
PIO's window.

**The renderer has changed under all of that and has not been on the glass.**
LVGL is out, `ui_draw.c` is in, and the build is clean with the 372 host checks
passing - but the board has been disconnected from USB since before the change,
so nothing has been flashed. Anything about how it looks is untested until
someone plugs it in.

What is still missing:

- **The live half of the renderer.** The needle, the value text and dirty
  rectangles are not written yet, so core 1 pushes the whole screen every
  frame - 434 KB, about 9 ms on the wire. That is the deliberate worst case
  `RENDERER_PLAN.md` wants measured before rectangles shrink it.
- **Real telemetry.** No Toyotune board has been on the bus with this yet, so
  gauges read `--` and the console says `LINK DOWN`. Milestone M4 - whether
  can2040 survives the panel's DMA bursts - is still open, and a full-screen
  push every frame is the worst case it will face.
- **Flash-backed page persistence.** `Pages_Init()` already takes a restored
  index; `main.c` still passes `0xFF`.
- The heartbeat's **page byte**, reserved and sent as zero.

With `-DDASH_HAVE_PANEL=OFF`, core 1 prints what it *would* draw over USB
serial instead of drawing it, which is enough to exercise the store and the
page tables with no glass attached.

## 8. Before touching the display bring-up

Read `firmware/vendor/README.md` first. It lists eight findings in Waveshare's
drivers, and **two of them present as a board that enumerates over USB and
prints absolutely nothing at all**:

- `QSPI_PIO_Init()` ends by disabling the PIO state machine, and nothing in
  the files they ship re-enables it. The first register write is a
  `pio_sm_put_blocking()` that then never returns. `src/panel.c` calls
  `QSPI_4Wrie_Mode(&qspi)` immediately after it for this reason - do not
  remove that line.
- An I2C write with `nostop` set that NAKs leaves the bus without a STOP, and
  the next transfer blocks forever. That hang lands in the touch path on core 1,
  before the first status line can be printed. Every touch transfer in
  `panel.c` is bounded by a timeout.

So on this board **silence is the normal symptom of a hang on core 1**, not of
a dead board or a serial problem - core 0 keeps servicing USB either way, so
the port still enumerates. Check DTR (section 4a) once; if the port is there
and still silent, suspect core 1 - and core 0 now polls `Panel_Stage()` and
prints the bring-up step core 1 stalled on, so the silence should name itself.

Three more things in the display path that survive any renderer, because they
are the panel's behaviour rather than the toolkit's:

- **Chip select must not go high when the DMA completes.** A finished DMA only
  means the last byte reached the PIO FIFO, not that it has been clocked out.
  The FIFO is drained first, under a bounded spin.
- **The CO5300 takes its column window in 2-pixel units.** An odd column start
  is rounded by the panel and the strip is drawn a pixel out, so every window
  goes through `Panel_RoundArea()` first.
- **The touch controller's command mode is a trap.** Reading the chip type
  needs it, and in command mode the controller answers with configuration
  instead of touch reports - so leaving it there means touch never works at
  all, which is exactly what made the first working display have a dead touch
  panel. `Panel_Init()` resets the part a second time to get back out, because
  the vendor driver has no exit: their command-mode write was inert until
  finding 5 was fixed. Separately, their latch-a-press-per-interrupt handler
  makes a swipe unreadable as anything but a tap, so `Panel_TouchService()`
  holds the press across the finger's travel and ages it out on a report gap.
