# Dash node firmware

RP2350 firmware for one gauge of the three-node dash cluster. The design
decisions behind it — and the reasoning that is expensive to rediscover — are
in [`../PLAN.md`](../PLAN.md); this file is only how to build and what is
here.

**Status: skeleton, no panel.** Everything below the display driver is written
and tested; the display driver itself is not, because the board has not
arrived and writing it against a datasheet would be guesswork.

## Build

The SDK, toolchain, CMake and Ninja all come from the Pico VS Code extension's
install under `~/.pico-sdk` — none of them are on PATH, which is the first
thing that stops a plain shell build. `CMakeLists.txt` finds them itself, so:

```
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.2.0
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/14_2_Rel1
~/.pico-sdk/cmake/v3.31.5/bin/cmake -B build -G Ninja \
    -DCMAKE_MAKE_PROGRAM=~/.pico-sdk/ninja/v1.12.1/ninja.exe
~/.pico-sdk/cmake/v3.31.5/bin/cmake --build build
```

Output is `build/dash_node.uf2` and `.elf`.

**One gotcha worth knowing:** RP2350 needs `picotool` to produce the UF2, and
the SDK will otherwise build it from source — which needs a *host* compiler,
not the ARM one, and fails on a machine where only `arm-none-eabi` is on PATH.
`CMakeLists.txt` points at the extension's prebuilt picotool so that never
happens.

## Host tests

The decode, the signal store, node identity, page selection and the UI model
have no hardware dependency, deliberately — so they are tested natively:

```
python test/run_tests.py
```

That is the same arrangement as the SAMC21 firmware's tests, and for the same
reason: these are the places where a mistake is silent. A wrong byte offset or
a missed sign bit gives a gauge that reads plausibly and wrongly, which is
exactly the failure that survives a bench test.

## External dependencies

Neither is vendored — pinning a copy here would hide which revision is in use.
Clone them beside the repo, or pass `-DCAN2040_PATH=` / `-DLVGL_PATH=`:

```
git clone https://github.com/KevinOConnor/can2040        external/can2040
git clone -b release/v8.3 https://github.com/lvgl/lvgl   external/lvgl
```

The firmware builds without either and says so on the console rather than
silently doing nothing. That is what lets the parts that need no bus and no
panel be worked on meanwhile.

**LVGL 8.x, not 9.** Waveshare's CO5300 panel driver and CST9217 touch glue
are written against 8.1; porting both is deliberate work, not a free upgrade.

## What is here

| File | |
|---|---|
| `signals.[ch]` | Every value a gauge can show, its units and its period |
| `telemetry.[ch]` | Frame decode, table driven, matching `toyotune.dbc` |
| `signal_store.[ch]` | Seqlock storage across the two cores, plus staleness |
| `node_id.[ch]` | Which gauge this board is, from a resistor divider |
| `pages.[ch]` | The page list, startup assignment, and the fault takeover |
| `ui_model.[ch]` | What a widget should show — tested, LVGL-free |
| `ui_lvgl.[ch]` | The LVGL binding — mechanical, needs LVGL |
| `can_link.[ch]` | can2040 setup, receive callback, node heartbeat |
| `main.c` | Boot and the core split |

### The split that matters

Core 0 runs can2040 and decode; core 1 renders. can2040 decodes the bus in a
PIO interrupt and is sensitive to interrupt latency, so keeping the renderer
off that core means a long draw cannot delay a CAN bit.

`ui_model.c` is separate from `ui_lvgl.c` for the same kind of reason. The
interesting decisions in a gauge are not the drawing — they are what counts as
stale, where a needle sits when the value is off-scale, and what a widget
shows before any frame has arrived. Those are decided in code that builds and
is tested on a host; the LVGL file is left as a mechanical translation small
enough to confirm by eye.

## What is deliberately missing

- **The panel driver.** The CO5300 QSPI flush callback and the CST9217 touch
  read callback are the only genuinely hardware-specific parts, and they come
  from Waveshare's driver. Everything above them is written.
- **Page persistence.** The selected page should survive an ignition cycle,
  written to flash only on change. `Pages_Init()` already takes a restored
  index; nothing stores one yet.
- **The heartbeat's page byte.** Reserved and sent as zero, so adding
  coordinated paging later needs no protocol change.

## Open questions this code cannot answer

Both are in `PLAN.md` and both need the board:

1. Which of the five free GPIOs are usable — `can_link.h` assumes GPIO25/26,
   the SH1.0 UART pins, as the only pair shared with no on-board peripheral.
2. Whether can2040 survives the panel's DMA bursts. That is milestone M4, and
   it gates the PCB.
