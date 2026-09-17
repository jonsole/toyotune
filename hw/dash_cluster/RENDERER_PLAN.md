# Dropping LVGL — the dash node's own renderer

Sits alongside [`PLAN.md`](PLAN.md), which holds the design decisions for the
cluster as a whole; this covers one change to the display layer of the RP2350
node.

Every number below was measured on the board during the 2026-09-13/17 sessions
unless it says "estimated".

**Progress.** LVGL came out of the firmware in one go rather than over the
phases below — see §5, which now records what was done against what was
planned. The transport, the paletted buffer and the chunked push are written
and build clean; **nothing has been on the glass yet**, so every runtime figure
in §3 and §4 marked "estimated" still is.

---

## 1. Why

The dial is now a **pre-rendered image** (`src/dash_faces.c`, generated on the
PC by `tools/face_render`). What LVGL draws at runtime is one line and a few
glyphs over that picture. Measured cost of what remains:

| | |
|---|---|
| Render work per frame | 5.5 ms of a 16.8 ms scan |
| Frame rate | 57 fps, 1.9% of frames late |
| Flash | 1,393 KB total; ~260 KB of it LVGL |
| SRAM | 353 KB of 520 KB used: LVGL hot code in `.time_critical` ~115 KB, LVGL heap 128 KB (peak **use** 14 KB), draw buffer 91 KB |

Most of this session went on working *around* the library rather than being
served by it: `lv_scale` regenerating every tick for every redrawn area (two
thirds of a frame, now fixed by pre-rendering), `LV_USE_ASSERT_OBJ` costing
15-20%, invalidation caching we wrote ourselves in `UiLvgl_Update()`, flush
ordering and TE-sync rules, and the label/needle area interplay.

Two further findings make the replacement small:

- **The faces need only 151 distinct colours across both dials**, anti-aliasing
  included. A 256-entry palette is lossless, so a stored face halves to 195 KB
  and can live in SRAM.
- **Anti-aliasing is not needed for what is drawn live.** 466 px across 1.75"
  is 266 dpi, a 0.0955 mm pitch: one pixel subtends 0.47 arcmin at a 700 mm
  instrument viewing distance, against ~1 arcmin of human acuity. A hard-edged
  needle is a span fill per row - no coverage rasteriser, which was the one
  piece of real work in writing our own.

## 2. What goes, what stays

| File | Lines | LVGL refs | Fate |
|---|---|---|---|
| `src/ui_lvgl.c` | 841 | 157 | **deleted**, replaced by `ui_draw.c` |
| `src/panel.c` | 1252 | 61 | **kept**; loses the display/indev wrappers, keeps QSPI, DMA, TE sync, touch I2C, brightness, stats |
| `src/ui_gauge.c/.h` | 218 | 79 | **moves to the PC renderer** (`tools/face_render`), which keeps LVGL - `lv_scale` is a good dial *design* tool where performance is irrelevant. The geometry constants stay shared with the firmware. |
| `src/dash_faces.c/.h` | generated | 9 | regenerated as paletted data in a plain struct |
| `src/dash_font_*.c` | generated | 9 each | regenerated as plain glyph structs by `tools/gen_font.py` |
| `src/ui_model.c`, `pages.c`, `signals.c`, `signal_store.c`, `telemetry.c`, `can_link.c`, `node_id.c` | - | 0-1 | **untouched**. The model is already LVGL-free and host-tested; that split is what makes this swap cheap. |

**LVGL stays in the repo** and in `tools/face_render`. Only the firmware stops
linking it.

## 3. The design

### 3.1 Frame pipeline

Per frame, unchanged in shape from today: work out what moved, redraw only
that, send it starting at the TE pulse.

```
   dirty rects  ->  for each 8-line chunk:
                        restore background   (paletted face -> 8-bit scratch)
                        span-fill needle     (palette index, no blending)
                        blit glyphs          (4 bpp over background)
                        convert 8 -> 16 bit  (256-entry LUT into 16-bit scratch)
                    DMA chunk N while the CPU builds chunk N+1
```

Two 8-line scratch buffers, 466 x 8 x 2 = **7,456 bytes each**, 15 KB total.

- **DMA per chunk: 159 µs** at the measured 46.9 MB/s.
- **Conversion per chunk: 56-75 µs estimated** (`ldrb`, `ldrh` from the LUT,
  `strh` per pixel at 200 MHz) - about half the transfer, so the CPU stays
  ahead and the DMA never stalls. **Phase 0 measures this.**

**Dirty rectangles, not whole frames.** A full-frame push is 434 KB, 9.3 ms,
a 55% bus duty cycle every frame - exactly the continuous DMA traffic that
milestone M4 (can2040 surviving the panel) is worried about. Dirty rects keep
it near 0.5 ms. Revisit only if M4 passes comfortably.

### 3.2 The paletted face

- The PC renderer emits, per face, an 8-bit index image plus a **shared
  256-entry RGB565 palette**, and **fails the build** if the faces between them
  exceed 256 colours.
- Stored face: **195 KB each** (447x447), 390 KB for both, against 781 KB now.
- Held in **SRAM** once LVGL's ~234 KB is freed. Background restore then runs
  at the measured 329 MB/s instead of 14-18 MB/s from XIP flash: **~40-60 µs
  per frame instead of 800-1500 µs**, which is the largest per-frame cost left
  today.

### 3.3 The needle

Hard-edged, drawn as palette indices straight into the 8-bit scratch: for each
row of the dirty rect, compute the span from the two long edges and fill it.
Geometry comes from `ui_gauge.h` exactly as now, including the sub-degree angle
interpolation and the smoothing in `UiModel_NeedleStep()`.

If the moving staircase shimmers in use (a sweeping edge is more visible than a
static one), blend **only the end pixel of each row's span** - about 160 pixels
a frame. Decide on the glass, not in advance.

### 3.4 Text

Keep the 4 bpp fonts `tools/gen_font.py` already generates, blended over the
restored background: the value is ~5k px a frame, about 0.1 ms, and text is
where hard edges show most. `gen_font.py` gains a plain-struct output format
(no `lv_font_fmt_txt`) and, optionally, 1 bpp for a 4x smaller table.

### 3.5 Touch, pages, transitions

- **Touch:** `Panel_TouchRead()` already talks to the CST9217 over I2C with
  timeouts; it loses only its `lv_indev` wrapper.
- **Gestures:** compare press and release positions - a horizontal move beyond
  a threshold is a swipe. The deliberate-gesture rules in `UiLvgl_HandleGesture()`
  carry over.
- **Page change:** a straight cut, plus a full redraw of the new face. The
  crossfade goes. If a transition is wanted later, a wipe costs one extra
  dirty rect per frame.

## 4. Budgets

Predicted, then measured from `arm-none-eabi-size -A` once LVGL was out:

| | LVGL (9c675c7) | predicted | measured |
|---|---|---|---|
| Flash | 1,265 KB | ~740 KB | **846 KB** |
| SRAM | 356 KB | ~215 KB | **246 KB** |
| Render work/frame | 5.5 ms | 2-3 ms | not yet on glass |

Both land short of the prediction for the same reason: the faces are still
stored as 16-bit images and palettised at boot (phase 1 outstanding), so the
390 KB of flash and the 195 KB of SRAM that paletting the *stored* face would
save have not been taken yet.

The SRAM figure is the striking one: it fell by 110 KB **while adding the
212 KB back buffer**. LVGL's heap, its hot code copied into `.time_critical`
and the 91 KB partial draw buffer together cost more than a whole
framebuffer does.

| | bytes, LVGL | bytes, now |
|---|---|---|
| `.text` | 365,896 | 42,280 |
| `.rodata` | 899,488 | 803,744 |
| `.data` | 123,496 | 4,364 |
| `.bss` | 238,408 | 244,940 |

## 5. Phases

The plan was to keep LVGL behind a build flag until the native path reached
parity, so there was always an A/B. That is not how it went: LVGL came out in
one pass, on the reasoning that a renderer which cannot show a dial is obvious
in a second and does not need a control to compare against, and that carrying
two display paths through the interesting part - the needle - would cost more
than it protected. What that gives up is the measured before/after on the same
build, so the old figures below are from the LVGL build as it stood at 9c675c7.

**Done**

- **The transport is LVGL-free.** `panel.c` keeps the QSPI, the DMA, the TE
  sync, the touch I2C and every vendor correction; it lost the display driver,
  the input device and the render-event plumbing. `Panel_TouchRead()` (an
  `lv_indev` callback) became `Panel_TouchService()` + `Panel_TouchDown()`.
- **The paletted back buffer**, `ui_draw.c`: 466x466 8-bit, one 256-entry
  table. Faces are palettised into it at boot rather than on the PC - a
  linear search with a run cache, reported by `UiDraw_LoadUs()` - so phase 1
  below is still outstanding. It fails loudly over 256 colours rather than
  quantising.
- **The chunked push**, `Panel_PushPaletted()`: one window, one chip select,
  8 lines converted at a time into one of two scratch buffers while the DMA
  clocks out the previous chunk. The first chunk is converted before the wait
  and started **by the TE interrupt itself**, so the frame begins at the pulse
  rather than when core 1 gets round to noticing it.
- **The faces are plain structs.** `DashImage_t` replaced `lv_image_dsc_t`;
  `build_faces.py` emits it directly.
- **`ui_gauge.c` and its two fonts moved to `tools/face_render/`**, where LVGL
  still lives and still costs nothing. `src/ui_gauge.h` stayed behind, now
  LVGL-free: it is the shared geometry, and the firmware needs it to land a
  live needle on pre-rendered graduations.
- **The build has no LVGL in it.** `DASH_HAVE_LVGL` became `DASH_HAVE_PANEL`;
  the firmware needs no LVGL checkout at all. 372 host checks still pass.

**Outstanding**

- **Phase 0 - measure, on the board.** Everything above is untested on glass.
  The two questions are whether the panel accepts a pixel stream split across
  several DMA transfers with chip select held, and what the conversion actually
  costs per chunk. `Panel_Push()` reports both: `LastConvertUs` against
  `LastBlockedUs`, and `Starved` for chunks whose DMA had already finished.
- **Phase 1 - paletted faces from the PC.** Move the palettising into
  `build_faces.py` so the stored face halves to 195 KB and boot does no work.
- **Phase 3 - the needle and the text.** Span-fill needle, glyph blit, dirty
  rectangles. `dash_font_value_56.c` was deleted with the rest of the
  `lv_font_t` data and `gen_font.py` needs a plain-struct output before text
  comes back.
- **Phase 4 - touch, swipe and the warning page** on the native path.

## 6. Tests

- **Host tests** (`test/run_tests.py`) gain the new pure code: needle span
  geometry for a given angle, palette conversion against a reference, glyph
  blit into a buffer, and dirty-rect arithmetic. These are the parts that can
  be wrong without looking wrong.
- **On the glass:** needle sweep at 60 fps, page swipe, stale/no-data states,
  the fault takeover, and a close look at the needle's edge while moving.
- **Counters stay:** frames, late frames, needle-synced flushes, work per
  frame, bus MB/s. They are how Phase 3's A/B is judged.

## 7. Risks

| Risk | Retired by |
|---|---|
| can2040 bit errors from DMA traffic (M4) | dirty rects, not whole frames; the same counters M4 uses |
| Panel rejects a burst split into chunks | Phase 0, before anything is built on it |
| Moving needle edge shimmers | end-pixel blend, decided on the glass |
| Text looks worse than LVGL's | keep 4 bpp blended glyphs |
| Losing LVGL's object asserts as a safety net | host tests over the pure code, plus our own asserts on the few pointers involved |
| A regression nobody notices until the car | **not retired.** The A/B this relied on was given up when LVGL came out in one pass (§5). What stands in for it is that the failures available to this renderer are loud - a blank screen, a wrong colour, a stalled needle - rather than subtle |
| A future face wants charts, bars or arcs | those widget types are in the page tables but unused; they would have to be written. Accepted. |

## 8. Accepted losses

The page crossfade, LVGL's widget set (`WIDGET_GRAPH`, `WIDGET_BARGRAPH`,
`WIDGET_ARC` are declared but unused), and its layout engine. The dial design
tool is **not** lost - it moves to the PC, where `lv_scale` costs nothing.

## 9. Open decisions

1. 4 bpp or 1 bpp glyphs (4 bpp assumed above).
2. Whether the needle gets the end-pixel blend - decide by looking.
3. Whether the paletted face lives in SRAM (fast, needs LVGL gone) or stays in
   XIP flash (slower, frees 195 KB). SRAM assumed.
4. Whether a page transition is wanted at all after the crossfade goes.
