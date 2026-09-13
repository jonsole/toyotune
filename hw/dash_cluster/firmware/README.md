# Dash node firmware

RP2350 firmware for one gauge of the three-node dash cluster. The design
decisions behind it — and the reasoning that is expensive to rediscover — are
in [`../PLAN.md`](../PLAN.md); this file is only how to build and what is
here.

**Status: running on the board, no telemetry yet. The LVGL 9 port builds
clean and passes the host tests, but has not been seen on hardware** because
the board was disconnected before it could be flashed. The panel is up: LVGL
renders the page tables through partial draw buffers to the CO5300, the
CST9217 identifies itself on I2C, and the node reports itself over USB serial
every two seconds. What has not been exercised is the other end — no Toyotune
board has been put on the bus with this yet, so every gauge currently reads
`--` and the console says `LINK DOWN`. Milestone M4, whether can2040 survives
the panel's DMA bursts, is still open and still gates the PCB.

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

Neither is vendored — both are maintained projects with public git history, so
a copy here would only hide which revision is in use. `external/` is
gitignored; clone them from the repo root, or pass `-DCAN2040_PATH=` /
`-DLVGL_PATH=`:

```
git clone https://github.com/KevinOConnor/can2040     external/can2040
git clone -b v9.5.0 https://github.com/lvgl/lvgl      external/lvgl
```

Built against:

| | revision | |
|---|---|---|
| LVGL | `85aa60d18b3d5e5588d7b247abf90198f07c8a63` | 9.5.0 |
| can2040 | `2988d4f11d8bff93f5a3d317fcd5e384a6aa3481` | master, 2026-09-12 |

The firmware builds without either and says so on the console rather than
silently doing nothing. That is what lets the parts that need no bus and no
panel be worked on meanwhile — and without LVGL, core 1 prints what it would
have drawn instead of drawing it.

`lv_conf.h` lives here rather than next to the LVGL checkout, and is
deliberately minimal: LVGL supplies a default for every option it knows about,
so each line in that file is a decision rather than an inherited template. The
setting that will silently ruin the display if changed is `LV_COLOR_DEPTH`,
together with `LV_DRAW_SW_SUPPORT_RGB565_SWAPPED`; `panel.c` has an `#error` on
both.

### Moving from LVGL 8 to 9

Only `panel.c` and `ui_lvgl.c` bind to LVGL, and the vendor drivers do not:
their LVGL glue was never used, only their panel and touch code. So the port
was contained, but plenty that was configuration in v8 is a runtime call in v9:

| v8 | v9 |
|---|---|
| `lv_disp_drv_t` + `lv_disp_drv_register` | `lv_display_create` + setters |
| `lv_disp_draw_buf_init`, size in **pixels** | `lv_display_set_buffers`, size in **bytes** |
| `lv_color_t` draw buffers | `uint8_t` buffers. `lv_color_t` is RGB888 in v9 and is no longer the pixel type |
| `LV_COLOR_16_SWAP` | `LV_COLOR_FORMAT_RGB565_SWAPPED` via `lv_display_set_color_format` |
| `disp_drv.rounder_cb` | an `LV_EVENT_INVALIDATE_AREA` handler |
| `disp_drv.monitor_cb` | timed between `LV_EVENT_RENDER_START` and `LV_EVENT_RENDER_READY` |
| `LV_TICK_CUSTOM` | `lv_tick_set_cb()` at runtime |
| `lv_coord_t` | `int32_t` |
| `LV_USE_PERF_MONITOR` | the same, but gated behind `LV_USE_SYSMON` |

Three traps, each of which stops the build or the display dead:

- **`lv_conf.h` may not `#include` anything unguarded.** v9 preprocesses it
  from assembly as well, because its Helium blend routine is a `.S`, so a C
  header there is handed to the assembler. LVGL's own template says to wrap any
  include in an `__ASSEMBLY__` guard. v8 tolerated the bare `<stdint.h>` that
  used to be at the top of this file because it had no assembly sources.
- **v9 links its examples into the `lvgl` target itself**, with
  `target_link_libraries(lvgl PUBLIC lvgl_examples)`. They are not an optional
  extra to be ignored: with widgets disabled they fail to compile and take the
  whole build with them. `CONFIG_LV_BUILD_EXAMPLES` and `_DEMOS` are forced
  off.
- **The byte order must not cost a pass.** v9's own header suggests calling
  `lv_draw_sw_rgb565_swap()` inside the flush, which is an extra sweep of every
  buffer. Rendering straight into `RGB565_SWAPPED` is free and is what
  `panel.c` does, since the software blender has a real destination path for
  it.

**Flash grew from 313 KB to 575 KB on the move.** The bulk is v9 compiling a
software blend path for every possible destination colour format:
`argb8888`, `rgb565`, `i1`, `al88`, `rgb888`, `l8` and more, around 82 KB of
them, where this build renders into exactly one. The `LV_DRAW_SW_SUPPORT_*`
options switch the unused ones off, but LVGL warns that some features reach for
particular formats internally, gradients for RGB888 and transparency for
ARGB8888, so it is a change to make with the panel in front of you rather than
on faith. There is no capacity problem at 575 KB of 4 MB, but smaller code
means less XIP cache pressure, which is what milestone M4 cares about.

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
| `panel.[ch]` | The CO5300 and CST9217, as an LVGL display and input device |
| `can_link.[ch]` | can2040 setup, receive callback, node heartbeat |
| `main.c` | Boot and the core split |
| `lv_conf.h` | LVGL configuration — only the settings that differ from default |
| `vendor/` | Waveshare's drivers, as delivered — see `vendor/README.md` |
| `test/psram_probe.c` | Standalone: is PSRAM fitted? (Answer: no) |

`panel.c` is where the vendor drivers are corrected rather than in
`vendor/`, so a future vendor release still diffs cleanly. Five of their
mistakes are written up in `vendor/README.md`; two of them present as a board
that enumerates over USB and prints absolutely nothing, so read that file
before changing anything in the display bring-up path.

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
   Note the published pinout image is now known to be wrong in two places
   (`vendor/README.md`, finding 3), so this wants the schematic.
2. Whether can2040 survives the panel's DMA bursts. That is milestone M4, and
   it gates the PCB. The panel currently issues around 180 flush DMAs a second
   with an idle bus; what that does to CAN bit timing is untested, because
   nothing has been on the bus with it yet.

## Reading the console

USB CDC discards everything printed before a host attaches, and this board
powers up with the ignition — so the boot banner is invisible to everyone in
practice. Anything worth knowing is repeated in a status line every two
seconds:

```
node 0  page 0  LINK DOWN  flush 2172  touch ok 0 @233,233
  node 0: no telemetry, none ever received
```

`flush` should climb, `flush-timeout` should never appear, `touch` should say
`ok`, and the count after it is presses — if it moves when a finger lands, the
whole I2C path works. The host must raise DTR or the firmware's output is
thrown away; see the `rp2350-build` skill.
