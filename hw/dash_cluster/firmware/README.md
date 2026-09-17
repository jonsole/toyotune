# Dash node firmware

RP2350 firmware for one gauge of the three-node dash cluster. The design
decisions behind it — and the reasoning that is expensive to rediscover — are
in [`../PLAN.md`](../PLAN.md), with the display layer's own rewrite in
[`../RENDERER_PLAN.md`](../RENDERER_PLAN.md); this file is only how to build
and what is here.

**Status: running on the glass without LVGL.** The node draws through its own
renderer - `src/ui_draw.c`, an 8-bit paletted back buffer the size of the
screen, with the needle (`ui_needle.c`) and the reading (`ui_text.c`) drawn
into it - and `src/panel.c` converts it to RGB565 on its way to the CO5300. It
builds clean under `-Wall -Wextra -Wconversion` and the 635 host checks pass.
Measured on the board with the simulator: the needle and a reading that
changes on nearly every frame go out as one rectangle per scan, 1.5-2.4 ms of
work in a 16.8 ms frame, with no late frames. `RENDERER_PLAN.md` §5 has the
figures.

What was already true before the change still is. No Toyotune board has been
put on the bus with this yet, so every gauge reads `--` and the console says
`LINK DOWN`. Milestone M4, whether can2040 survives the panel's DMA bursts, is
still open and still gates the PCB.

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

**can2040 is the only one the firmware has.** It is not vendored — it is a
maintained project with public git history, so a copy here would only hide
which revision is in use. `external/` is gitignored; clone it from the repo
root, or pass `-DCAN2040_PATH=`:

```
git clone https://github.com/KevinOConnor/can2040     external/can2040
```

The firmware builds without it and says so on the console rather than silently
receiving nothing. That is what lets the parts that need no bus be worked on
meanwhile. The display layer has its own switch, the CMake option
`DASH_HAVE_PANEL` (default ON); turn it off and core 1 prints what it would
have drawn instead of drawing it.

**LVGL is needed only to regenerate the faces**, not to build the firmware.
The dials are pre-rendered pictures, built on the PC by `tools/face_render`,
and that tool is the only thing left in this tree that links LVGL:

```
git clone -b v9.5.0 https://github.com/lvgl/lvgl      external/lvgl
python tools/face_render/build_faces.py
```

Built against:

| | revision | |
|---|---|---|
| can2040 | `2988d4f11d8bff93f5a3d317fcd5e384a6aa3481` | master, 2026-09-12 |
| LVGL (face renderer only) | `85aa60d18b3d5e5588d7b247abf90198f07c8a63` | 9.5.0 |

`lv_conf.h` still lives here rather than next to the LVGL checkout, because
`tools/face_render/lv_conf.h` wraps it: the dial is only a faithful picture of
what the board draws if the fonts, colour depth and every drawing option match,
so there is one configuration and the renderer overrides the two or three lines
that cannot apply on a PC. Nothing in `src/` reads it any more.

## The renderer

LVGL was doing very little at a real cost. The dial is a picture rendered once
on the PC; what the toolkit drew at runtime was a line and a few glyphs over
it, for 5.5 ms of a 16.8 ms scan, 260 KB of flash and 234 KB of SRAM between
its heap, its draw buffer and the hot code the build pulls into
`.time_critical`. `../RENDERER_PLAN.md` has the measurements and the argument;
the short version is that two findings made the replacement small — the faces
need only **151 distinct colours** across both dials, so a 256-entry palette is
lossless rather than a quantisation, and nothing drawn live needs
anti-aliasing at 266 dpi.

What came out of it, `arm-none-eabi-size -A` either side:

| | with LVGL | without |
|---|---|---|
| `.text` | 365,896 | 41,992 |
| `.rodata` | 899,488 | 404,608 |
| `.data` | 123,496 | 4,364 |
| `.bss` | 238,408 | 244,932 |
| SRAM | 364,224 B (356 KB) | 251,616 B (246 KB) |

**SRAM use fell by about 110 KB while gaining a 212 KB back buffer**, which is
the number that makes the whole thing worth doing: LVGL's heap, its
`.time_critical` code and the 91 KB partial draw buffer together cost more than
a full framebuffer does. Flash went from about 1,265 KB to about 440 KB, most
of what is left being the two 447x447 faces in `.rodata` - 195 KB each at a
byte a pixel.

The shape of it:

- **`src/ui_draw.c` — the back buffer.** 466x466 at one byte a pixel is 212 KB
  and fits; at RGB565 it would be 434 KB and would not. The faces arrive
  already in its format, so loading one is a copy per row.
- **`tools/face_render/build_faces.py` — rendering and palettising.** The
  dials are drawn by LVGL at 4x the panel's resolution, every render pixel is
  snapped to the design colour it belongs to (the colours are read from
  `src/ui_gauge.h`), and each panel pixel becomes its 4x4 block as one of 8
  shades between the two colours meeting there. Both dials together need 34
  entries of one shared 256-entry table; index 0 is always black, so a
  cleared buffer is a black screen. The script refuses to write a face that
  contains colours nobody designed, and checks the table's byte order the way
  the firmware reads it. `RAMP_LEVELS` sets the shades per edge.
- **`Panel_PushPaletted()` — the way out.** It sends a rectangle of palette
  indices under a **single window command and a single chip select**,
  converting 8 lines at a time into one of two small scratch buffers while the
  DMA is still clocking out the previous chunk. At the measured 46.9 MB/s a
  chunk is 159 µs on the wire against a conversion of a byte load, a lookup and
  a halfword store per pixel, so the CPU stays ahead; `PanelPush_t.Starved`
  counts the chunks where it did not and the bus went idle waiting.
- **The first chunk is started by the TE interrupt itself.** It is converted
  before the wait, so the frame begins at the pulse rather than whenever core 1
  next notices one. Sending a frame that is not aligned to the panel's scan is
  what tearing is, and `Panel_WaitTe()` exists for a caller that wants the
  cadence without sending anything.

What the native renderer does **not** do yet: the needle and the value text are
not drawn, so core 1 currently pushes the whole screen every frame — the dial
and nothing over it. That is deliberately the most expensive thing this
pipeline will ever be asked to do, 434 KB and about 9 ms a frame, and it is the
measurement wanted before dirty rectangles put it back near half a millisecond.
`src/dash_font_value_56.c` went with LVGL; `tools/gen_font.py` still emits
LVGL's `lv_font_fmt_txt` and gains a plain-struct format when live text is
written.

## What is here

| File | |
|---|---|
| `signals.[ch]` | Every value a gauge can show, its units and its period |
| `telemetry.[ch]` | Frame decode, table driven, matching `toyotune.dbc` |
| `signal_store.[ch]` | Seqlock storage across the two cores, plus staleness |
| `node_id.[ch]` | Which gauge this board is, from a resistor divider |
| `pages.[ch]` | The page list, startup assignment, and the fault takeover |
| `ui_model.[ch]` | What a widget should show — tested, display-free |
| `ui_draw.[ch]` | The paletted back buffer and its palette, restoring the face, and the live colours |
| `ui_needle.[ch]` | Placing and drawing the needle, hard-edged - host-tested |
| `ui_graph.[ch]` | The strip chart: history and plot drawing - host-tested |
| `ui_graphpage.[ch]` | A graph page: its history, plot and readings |
| `ui_gmeter.[ch]` | The g-force meter's arithmetic and zeroing - host-tested |
| `ui_gpage.[ch]` | The g-force page: dot, trail, peaks and readings |
| `imu.[ch]` | The QMI8658 accelerometer, on the touch controller's I2C bus |
| `ui_text.[ch]` | Drawing text into the buffer, blended by palette index - host-tested |
| `dash_font.h`, `dash_font_value_56.c` | The firmware's font format, and the reading's font - generated by `tools/gen_font.py --format plain` |
| `ui_gauge.h` | Dial and needle geometry — shared with the face renderer, and LVGL-free, because the needle is to be drawn live and has to land on the pre-rendered graduations to the pixel |
| `dash_faces.[ch]` | The pre-rendered dials. **Generated** — see `tools/face_render` |
| `panel.[ch]` | The CO5300 and CST9217: QSPI/PIO/DMA transport, TE sync, touch |
| `can_link.[ch]` | can2040 setup, receive callback, node heartbeat |
| `main.c` | Boot and the core split |
| `lv_conf.h` | LVGL configuration — for `tools/face_render` alone |
| `tools/face_render/` | The PC tool that draws the dials, and the only LVGL left: `ui_gauge.c`, its two fonts, and `ui_gauge_scale.h` |
| `tools/gen_font.py` | Bitmap fonts from a TTF |
| `vendor/` | Waveshare's drivers, as delivered — see `vendor/README.md` |
| `test/psram_probe.c` | Standalone: is PSRAM fitted? (Answer: no) |

`panel.c` is where the vendor drivers are corrected rather than in
`vendor/`, so a future vendor release still diffs cleanly. Eight findings are
written up in `vendor/README.md`; two of them present as a board
that enumerates over USB and prints absolutely nothing, so read that file
before changing anything in the display bring-up path.

Two corrections are worth naming here because they outlived LVGL and are easy
to undo:

- **Chip select must not be raised when the DMA finishes.** A finished DMA
  only means the last byte reached the PIO FIFO, not that it has been clocked
  out. The vendor's completion handler raises CS there; ours drains the FIFO
  first, under a bounded spin — `Panel_DrainSpinsMax()` is the evidence that
  the bound is generous rather than lucky.
- **The CO5300 takes its column window in 2-pixel units**, so an odd column
  start is rounded by the panel and the strip lands a pixel out. Every window
  goes through `Panel_RoundArea()`, which rounds starts down and ends up so
  widths and heights come out even too. The row rounding was originally there
  for LVGL's banding and is kept: the column rule is the panel's own.

### The split that matters

Core 0 runs can2040 and decode; core 1 renders. can2040 decodes the bus in a
PIO interrupt and is sensitive to interrupt latency, so keeping the renderer
off that core means a long draw cannot delay a CAN bit.

`ui_model.c` is separate from `ui_draw.c` for the same kind of reason. The
interesting decisions in a gauge are not the drawing — they are what counts as
stale, where a needle sits when the value is off-scale, and what a widget
shows before any frame has arrived. Those are decided in code that builds and
is tested on a host; the drawing is left small enough to confirm by eye.

## What is deliberately missing

- **The live half of the renderer.** The needle, the value text and dirty
  rectangles, in that order — see `../RENDERER_PLAN.md`. Until they exist the
  dial is static and the whole screen is sent every frame.
- **Touch gestures.** `Panel_TouchService()` and `Panel_TouchDown()` hold a
  press across the finger's travel, which is what a swipe needs and what the
  vendor's latch-per-interrupt handler could not do, but nothing measures
  press against release yet.
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
   it gates the PCB. What competes with can2040 is the share of wall-clock time
   the panel is mid-burst, not the frame rate, which is why
   `Panel_FlushBusyUsPerFrame()` is measured rather than derived — and a
   full-screen push every frame is the worst case it will ever face.

## Reading the console

USB CDC discards everything printed before a host attaches, and this board
powers up with the ignition — so the boot banner is invisible to everyone in
practice. Anything worth knowing is repeated in a status line every two
seconds. Its shape, with illustrative values — this build has not been run, so
these are not a capture:

```
node 0  page 0  LINK DOWN  flush 2172  touch ok rep 0 press 0 @233,233
  int 0 up / 0 down
  push 118  chunks 6844  starved 0  last 9300us = convert 3100 + blocked 6200
  te on  edges 7104  period 16667us  waits 118  timeouts 0  avg wait 900us  late 0
  bench us 1098000 px 25612792
  rounded 0  overlap dma 0 cs 0
  bus 46.9 MB/s  9300us/frame  drain<=3
  node 0: no telemetry, none ever received
```

`flush` and `push` should climb, `flush-timeout` should never appear, `te`
should say `on` with `period` near 16,667 µs, and `touch` should say `ok` —
the count after it is presses, and if it moves when a finger lands, the whole
I2C path works.

The two push figures are the ones to read together. A frame held down by
`blocked` is bus-bound and only a smaller rectangle will help; one held down by
`convert` with `starved` climbing is CPU-bound, and the conversion loop is
where to look.

The host must raise DTR or the firmware's output is thrown away; see the
`rp2350-build` skill. And if core 1 wedges during bring-up, core 0 prints the
stage it wedged on rather than leaving a silent board — that silence used to be
the symptom of every panel fault in this project.
