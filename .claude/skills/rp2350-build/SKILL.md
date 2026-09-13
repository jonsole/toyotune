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
from the code alone. One exception: PLAN.md still describes staying on LVGL
8.1, which was the plan at bring-up. The code has since moved to **LVGL 9** -
`firmware/README.md` §"Moving from LVGL 8 to 9" is current, PLAN.md is not.

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
git clone -b v9.5.0 https://github.com/lvgl/lvgl         external/lvgl
```

`external/` is beside the repo root - or pass `-DCAN2040_PATH=` /
`-DLVGL_PATH=`. Watch the configure output: two `CMake Warning` blocks mean
you built a node that cannot receive and cannot draw.

**LVGL 9.5.0**, the version `CMakeLists.txt` names. Waveshare's CO5300 and
CST9217 drivers are written against 8.1; `panel.c` is our own binding and
targets v9 directly, so the vendor's LVGL 8 glue is not used. An 8.x checkout
will not build - `ui_lvgl.c` uses `lv_scale`, which is v9-only.

**`lv_conf.h` options must be v9 names.** v9 renamed many of them, and a name it
does not know is silently ignored rather than rejected. `LV_USE_ARM2D` is one:
nothing in 9.5.0 reads it, so `#define LV_USE_ARM2D 1` does nothing at all. The
v9 option is `LV_USE_DRAW_ARM2D_SYNC`, and it needs the Arm-2D library, which
is not in the tree - so enabling it should fail the build until that is added.
Arm-2D also gets most of its speed from Helium (Cortex-M55/M85); the RP2350's
M33 has none, so measure before taking the dependency. To check any option,
grep for it in `external/lvgl/src/lv_conf_internal.h` - if it is not there, v9
ignores it.

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
| `can2040 not found` / `LVGL not found` | §3, and expected on a fresh clone |
| Undefined `lv_*` symbols, or `lv_scale` unknown | LVGL was found at configure time but is an 8.x checkout - the binding targets v9.5.0 |
| An `lv_conf.h` option has no effect | It is a v8 name, or not an LVGL option at all; v9 ignores unknown names silently - §3 |
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

The board arrived 2026-09-12 and the firmware runs on it, drawing on the
glass: the CO5300 flush and the CST9217 touch read are both implemented in
`src/panel.c`, and the needle gauges animate without tearing now that each
flush is synced to the panel's TE pulse and sent as one burst. The node
identity falls back safely with no divider fitted. Confirmed as
**RP2350 rev A2, QFN60** - i.e. RP2350A, GPIO0..29, so every pin is inside
PIO's window.

What is still missing:

- **Real telemetry.** No Toyotune board has been on the bus with this yet, so
  gauges read `--` and the console says `LINK DOWN`. Milestone M4 - whether
  can2040 survives the panel's DMA bursts - is still open.
- **Flash-backed page persistence.** `Pages_Init()` already takes a restored
  index; `main.c` still passes `0xFF`.
- The heartbeat's **page byte**, reserved and sent as zero.

Without an LVGL checkout, core 1 prints what it *would* draw over USB serial
instead of drawing it, which is enough to exercise the store and the page
tables with no glass attached.

## 7. Before touching the display bring-up

Read `firmware/vendor/README.md` first. It lists five mistakes in Waveshare's
drivers, and **two of them present as a board that enumerates over USB and
prints absolutely nothing at all**:

- `QSPI_PIO_Init()` ends by disabling the PIO state machine, and nothing in
  the files they ship re-enables it. The first register write is a
  `pio_sm_put_blocking()` that then never returns. `src/panel.c` calls
  `QSPI_4Wrie_Mode(&qspi)` immediately after it for this reason - do not
  remove that line.
- An I2C write with `nostop` set that NAKs leaves the bus without a STOP, and
  the next transfer blocks forever. The hang lands in LVGL's input callback,
  before the first status line can be printed. Every touch transfer in
  `panel.c` is bounded by a timeout.

So on this board **silence is the normal symptom of a hang on core 1**, not of
a dead board or a serial problem - core 0 keeps servicing USB either way, so
the port still enumerates. Check DTR (section 4a) once; if the port is there
and still silent, suspect core 1.
