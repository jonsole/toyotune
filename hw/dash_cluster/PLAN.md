# Dash cluster — three RP2350 CAN gauge nodes

**Status: ECU side done and on the bench; node firmware running on real
hardware; no panel yet.** Written 2026-09-04 as a hand-off document, and kept
current since.

The ECU-side work (§3) is complete, flashed and verified at 998 and 3500 rpm.
The dev board arrived 2026-09-12; `firmware/` builds for RP2350, boots, reads
its identity and reports through the page tables over USB serial. What remains
before a gauge draws anything is the CO5300 panel driver, vendored in
`firmware/vendor/` and not yet ported.

The goal is an in-dash display of the ECU data the Toyotune boards already put
on CAN: **three circular displays of roughly 2 inches diameter**, each showing
several signals at once as a mix of dials, rolling graphs and numerics.

---

## 1. Orientation — read this first in a new session

The repo-level guide is `CLAUDE.md` at the root. The parts that matter here:

| What | Where |
|---|---|
| Telemetry frame/signal tables | `hw/toyotune_lv_2p1/sw/samc2x/toyotune_denso/can_telemetry.c` |
| CAN identifiers, per-CPU/per-ECU config | `.../toyotune_denso/config.h` |
| Inter-CPU DMA block layouts (the data source) | `.../toyotune_denso/ecu.h` |
| CAN driver | `.../toyotune_denso/can.c` |
| DBC consumed by host tooling | `.../toyotune_denso/toyotune.dbc` |
| Host CAN decoder | `hw/toyotune_lv_2p1/sw/python/can_monitor.py` |
| ADC/sensor reverse-engineering notes | `roms/3S-GTE/gen3/adc_system.md` |
| Bench rig skills | `.claude/skills/can-check`, `.claude/skills/stimulator` |

Facts that are easy to get wrong:

- **The Denso ECU is big-endian; the SAMC21 and RP2350 are little-endian.**
  `ecu.h` provides `ECU_Be16()`. Today `can_telemetry.c` byte-copies fields
  straight out of the DMA block, so 16-bit signals happen to land big-endian
  on the wire, matching the DBC's `@0+` Motorola byte order. **Once the
  firmware starts computing values, that accident stops protecting you** —
  every scaled value must be written to the payload big-endian explicitly.
- **Two boards, disjoint identifiers.** CPU1 telemetry is `0x400`–`0x404`,
  CPU2 is `0x420`–`0x424` (`TOYOTUNE_CAN_ID_TELEMETRY_BASE` in `config.h`).
  `0x40A`/`0x40B` and `0x42A`/`0x42B` are reserved for diag command/response.
  Both boards publish the *same signal names*; a dash node must pick a source
  per signal, not merge them blindly.
- **The bus is not only Toyotune boards.** A 14Point7 wideband lambda
  controller is also a transmitter on it (§4.9), and its identifiers were
  chosen to suit Megasquirt/Haltech/Link, not to avoid ours. Log the bus
  before assuming an identifier is free.
- **Nothing on the ECU side moves unless the bench stimulator is running.**
- **A dash node must ACK.** With one Toyotune board plus a dash node, the dash
  is the only other node on the bus. Built listen-only, it would leave the
  board error-passive with nothing acknowledging its frames.

---

## 2. Decisions already made

| Decision | Choice | Why |
|---|---|---|
| Topology | **Three independent nodes**, each MCU + panel + transceiver | Build one, debug it, replicate twice. A dead gauge is one replaceable unit. |
| Node board | **Waveshare RP2350-Touch-AMOLED-1.75** | Integrated RP2350 + 1.75" round AMOLED. pico-sdk is ARM GCC + CMake, the same shape as the existing SAMC21 tree; 520 KB SRAM; two cores; three PIO blocks. Skips writing a panel driver entirely. |
| CAN on the node | **can2040** (PIO software CAN), MCP2518FD as fallback | Full CAN 2.0B TX **and** RX — TX matters because ACK is a transmit action. RP2350 supported with pico-sdk 2.0.0+ and `-DPICO_RP2350`. |
| Panel | **1.75" round AMOLED, 466×466, CO5300 over QSPI** (on-board) | ~44.5 mm active against the ~51 mm target — the closest in the family. AMOLED brightness answers the behind-the-windscreen readability problem that sinks a ~300 nit IPS panel. |
| Rendering | **LVGL partial buffers, no full framebuffer** | 466×466×16bpp is 424 KB of 520 KB — too tight to hold whole. Partial rendering needs ~85 KB. See §4.1a. |
| Interaction | **Swipe left/right on the touch panel selects the page** | Turns three fixed faces into three windows onto a larger set of pages. Node ID picks the startup page, not the only page. See §4.6. |
| Transceiver | **TJA1051** | Already the known-good front end on the Toyotune board. |
| Node identity | **One ADC pin + resistor divider**, not a `#define` | One flash image for all three nodes; replication is a resistor change, not a different binary. Costs one pin instead of two on a board with only five free, and sidesteps erratum E9. See §4.5. |
| Unit conversion | **In the SAMC21 firmware**, before the value goes on the bus | See §3. |

---

## 3. Part A — move unit conversion into the SAMC21 firmware

### 3.1 Why here and not in the DBC

Today only three signals in `toyotune.dbc` are scaled at all (`Rpm` 0.1953125,
`Battery` 0.0775 V, `InjPw` 4 µs). Everything else is a raw count with factor
1, so a gauge would read "Coolant 58339".

The decisive reason to convert in firmware rather than in the DBC:

- **A DBC can only express `raw × factor + offset` — a straight line.** ECT,
  THA and THAM come from NTC thermistors and are *not* linear in ADC counts.
  No factor/offset can express them. They need a lookup table with
  interpolation, which only code can do.
- The ECU's internal encodings are strange (ECT is XOR-inverted and masked;
  THA/THAM are XOR 0xFF; RPM is ×5.12). Publishing those raw leaks ECU
  internals into the wire protocol, and every consumer has to re-implement
  the same undo.
- Three dash nodes, `can_monitor.py`, and any future logger all agree by
  construction rather than by three copies of the same arithmetic.

**The cost, and why it is acceptable:** the raw values are no longer directly
on the scaled frames. That is fine because **the `RAW` frame already carries
every byte of both DMA blocks unmodified** — 72 bytes in 11 slices at 50 ms,
so a complete raw refresh every ~550 ms. Reverse-engineering work keeps its
unfiltered view; consumers get engineering units. Keep both.

The DBC does **not** go away. It stays the single source of truth for
consumers — its factors just become trivial and honest (0.01, 0.1) instead of
unknown.

### 3.2 Where the code goes

Add a new module, **`ecu_scale.c` / `ecu_scale.h`**, containing pure functions
`raw -> engineering units` with no CAN knowledge:

```c
int16_t  ECU_ScaleEctC100(uint16_t RawEct);      /* 0.01 degC */
int16_t  ECU_ScaleIgnTimingDeg100(uint8_t Raw);  /* 0.01 deg BTDC */
uint16_t ECU_ScalePimKpa10(uint16_t RawPim);     /* 0.1 kPa absolute */
```

Reasons for a separate module rather than putting it in `can_telemetry.c`:
it is unit-testable on a host with no hardware, it is reusable by `diag.c` and
any debug output, and it keeps `can_telemetry.c` about framing.

**No floating point.** The SAMC21 is a Cortex-M0+ with no FPU. All conversions
are fixed-point integer maths; thermistors are lookup tables with linear
interpolation between entries.

**Per-family branching.** `ecu.h` is already branched on
`TOYOTUNE_ECU_MR2` / `TOYOTUNE_ECU_ST205`. Sensor curves may differ between
families, so `ecu_scale.c` must be able to branch the same way even if both
families initially share one table.

### 3.3 Extending the signal table

`CanTelemetry_Signal_t` in `can_telemetry.c` is currently a pure byte copy:

```c
Payload[Signal->Offset] = Block[Signal->Field];
if (Signal->Size == 2)
    Payload[Signal->Offset + 1] = Block[Signal->Field + 1];
```

Add a `Transform` field (an enum indexing a table of conversion functions,
with `CAN_XFORM_COPY` preserving today's behaviour for the flag bytes). This
keeps the module's stated philosophy intact — *"adding a signal is a row in
the table"* — rather than growing per-signal code.

**Write scaled values big-endian explicitly.** See §1.

### 3.4 Transport units

Principle: **choose transport resolution finer than the sensor's own
resolution, so the wire never becomes the limiting factor.** Uniform 16-bit
signals also make the decoder trivial and remove the need to revisit a
signal's width later.

| Signal | Source | Transport | Unit/bit | Conversion status |
|---|---|---|---|---|
| Rpm | `ECU_DmaData2_t.RpmX5p12` | `uint16` | 1 rpm | **Known** — raw / 5.12 |
| InjPw | `ECU_DmaData1_t.InjPwInj1` | `uint16` | 1 us | **Known** — raw x 4 |
| InjDuty | computed | `uint16` | 0.01 % | **Assumption** — derived from InjPw + Rpm (both already in FAST), assuming **one injection per two crank revolutions**. The disassembly has both `injector_update` and `injectors_batch_update` plus `var_inj_pw_inj1..4`; if the ECU batch-fires, true duty is double and `ECU_INJ_DUTY_DIVISOR` becomes 6000 |
| Battery | `Battery` | `uint16` | 0.01 V | **Known** — `0.0774 x + 0.0601`, see §3.6a |
| Pim / Pim2 | `Pim`, `Pim2` | `int16` | 0.1 kPa abs | **Known** — see §3.5. Signed: at the sensor's zero-pressure clamp the conversion is legitimately about -1.2 kPa, and unsigned would wrap it to 6553.5 kPa exactly when a fault puts it there |
| Tps | `Tps` | `uint16` | 0.01 % | **Needs work** — requires closed/WOT endpoints |
| Ect | `Ect` | `int16` | 0.01 degC | **Known** — curve in §3.6, `X = var_ect/256` |
| Tha / Tham | `Tha`, `Tham` | `int16` | 0.01 degC | **Known** — same curve, §3.6 |
| IgnTiming | `ECU_DmaData2_t.IgnTiming` | `int16` | 0.01 deg BTDC | **Ambiguous** — two conflicting pairs, see §3.6b |
| KnockRetard, per-cyl x3 | `KnockRetard`, `KnockRetardInfo[3]` | `int16` | 0.01 deg | **Partly known** — notes say ~0.5 deg/count, confirm |
| IscvDuty | `IscvDuty` | `uint16` | 0.01 % | **Needs work** |
| AdcLambda | `AdcLambda` | `uint16` | 0.001 V | **Needs work** — ADC reference scaling |
| LambdaTrim, FuelTrim, NvTrim* | various | `int16` | 0.01 % | **Needs work** — confirm zero point and sign |
| ErrorFlags1/2, Flags1, Flags46, LimiterFlags, PwLoopMode | various | `uint8` | raw | Pass through; describe bits as DBC value tables |

### 3.5 The PIM conversion — and a correction to `adc_system.md`

`roms/3S-GTE/gen3/adc_system.md` gives the MAP conversion as:

```
psi_gauge = ((((x*256/1.285156) + 10560)/65536*5) - 2.3293) / 0.1025
```

**That `x` is the XDF's 8-bit table byte, not the 16-bit `var_pim2`.** The
`x*256` term is what rebuilds the 16-bit value. Substituting `var_pim2`
directly for `x` gives answers wrong by a factor of ~1.285 and does not
reproduce that document's own table. For the 16-bit DMA field, use:

```
volts     = (var_pim2 / 1.285156 + 10560) / 65536 * 5
psi_gauge = (volts - 2.3293) / 0.1025
kpa_abs   = psi_gauge * 6.89476 + 101.325
```

Verified against all four rows of that document's own table: `0x0000` -> 0.806 V,
`0x2E4D` -> 1.51 V, `0x6443` -> 2.33 V (atmospheric), `0xDA00` -> 4.12 V
(the 17.5 psi boost-cut threshold). Implement in fixed point.

**Independently confirmed by `roms/3S-GTE/convert.xlsx`**, whose PIM rows read
`((((A/1.285156)+10560)/65536*5)-2.3293)/0.1025` — operating directly on the
16-bit value with no `x256`, exactly as above. That workbook also carries the
inverse (psi -> PIM value) if it is ever needed.

### 3.6 The temperature curves — solved

**Source: `roms/3S-GTE/convert.xlsx`, Sheet1 row 8.** That workbook is the
existing conversion reference for this ECU family and settles what was
previously the largest unknown here. It also carries battery, PIM and
ignition rows — see below.

One curve serves **all three** temperature sensors:

```
degC = 161.034800506902
     + 2639.01416123947   / ( 273.34227840271899 - X)
     + 94262.143539369397 / (-457.82340934181599 - X)
```

`X` is the **ECU value**, i.e. the post-`XOR 0xFF` figure, not the raw ADC:

- **THA / THAM** — `var_tha` / `var_tham` directly (8-bit).
- **ECT** — use the 16-bit value as fixed point, `X = var_ect / 256.0`. The
  low byte's masked top two bits are real resolution, so this gives 0.25-count
  steps rather than 1. (`var_ect` is both bytes XOR 0xFF with the low byte
  masked to `0xC0` — a 10-bit ADC left-justified. If you want the plain ADC
  instead: `adc10 = ((hi ^ 0xFF) << 2) | ((lo ^ 0xFF) >> 6)`.)

**Validated against three independent points:**

| Input | Curve output | Cross-check |
|---|---|---|
| THA `0x86` = 134 | 20.70 degC | `adc_system.md` says ~20 degC |
| ECT `0xE400`, X = 228 | 81.79 degC | `adc_system.md` says ~82 degC |
| `0xD8` = 216 | 67.17 degC | the sheet's own cell says 67.165 |

The first two are the sensor-fault defaults documented in `adc_system.md` from
the disassembly — derived independently of this spreadsheet, so the agreement
is real corroboration rather than circular.

**Validated against real measured data.**
`roms/3S-GTE/temp_sensor_calibration.xlsx` holds the bench calibration this
curve was fitted to: 84 points from a digital thermometer covering
**X = 0..238, i.e. -34.2 to +100.6 degC**. Against that set the curve gives:

| Metric | Value |
|---|---|
| Mean absolute error | **0.32 degC** |
| RMS error | **0.40 degC** |
| Worst single point | **1.21 degC** (X = 235) |

The measurement data's *own* repeatability is 1.0-1.4 degC — the same X value
appears with readings 1.0, 1.1 and 1.4 degC apart at X = 225, 227 and 236. So
**the fit's worst-case error is the size of the measurement noise. There is
nothing to gain by refitting.** Use the published formula as-is.

That workbook also has a 5-point alcohol-thermometer set (RMS 1.48 degC), an
earlier and cruder pass which disagrees with the digital set at the top end
(it reads X = 239 as 100.0 degC where the digital set has X = 238 at 100.6).
**Ignore the alcohol data; the digital set is the good one.**

**Implementation.** Two poles summed is awkward in fixed point on a
Cortex-M0+, so precompute. The clean answer is a **256-entry table of
`int16` degC x 100 (512 bytes)**, indexed directly by the integer ECU value:
THA/THAM then need no interpolation at all, and ECT interpolates only on the
low byte for its extra 0.25-count resolution. If table size ever matters, the
tradeoff is:

| Entries | Max deviation from the formula |
|---|---|
| 33 | 1.62 degC — too coarse |
| 65 | 0.49 degC |
| 129 | 0.14 degC |
| 257 | 0.04 degC |

Going finer than ~0.4 degC is pointless — that is the formula's own error
against the measured data.

**Range — a correction to earlier guidance in this document.** An earlier
draft said to clamp to `[0x03, 0xF8]` because the fit "diverges outside it".
The low half of that is wrong: **X = 0 is a measured point** (-34.2 degC
measured, -35.20 degC from the curve, error -1.00 degC), so no low clamp is
needed. The curve is monotonic and well behaved across the whole of X = 0..248.

The *high* end is the real caveat. Measured data stops at X = 238 (100.6 degC);
above that the curve climbs steeply toward its pole at X = 273.34:

```
X = 240  ->  105.1 degC
X = 248  ->  131.6 degC   (0xF8, the ECU's own ADC clamp)
X = 255  ->  172.7 degC   (not reachable - the ECU clamps first)
```

Those are plausible overheat values rather than nonsense, and the ECU's ADC
clamp keeps X at or below 248 anyway — but they are **unvalidated
extrapolation**. Display anything above ~238 as an overheat warning state
rather than trusting the number.

**Inverse.** The sheet also gives temperature -> ECU value as a logistic fit:

```
X = 309.31154914815198 / (1 + 1.19477713979554 * exp(-0.033769539695967403 * T))
    - 59.355301387609103
```

It round-trips to within ~0.3 degC over 0..100 degC but drifts by ~1.7 degC at
110 degC — it is an independent fit, **not an exact inverse**. Treat the
forward form as authoritative and use this only where an inverse is genuinely
needed.

Copy this curve into `adc_system.md`, which currently documents only the
encoding and the two fault defaults.

### 3.6a Battery — refine the constant

The same workbook, and `roms/3S-GTE/D151803-9661/battery_voltage_conversion.xlsx`
(which records it as MCU 9651, address `0x56`, with four fit points), give:

```
volts = 0.0774 * X + 0.0601
```

The firmware and DBC currently use `0.0775` with **no offset**, which reads
about 0.04 V high at 14.6 V. Minor, but free to fix while the scaling work is
open.

### 3.6b Ignition timing — the sheet is ambiguous, still open

`convert.xlsx` rows 13-15 hold a linear interpolation for ignition timing, but
**two conflicting calibration pairs sit side by side**:

- The pair the active formula actually interpolates: `(75, -5)` and
  `(85, -10)` — i.e. **-0.5 deg per count**.
- Adjacent unused cells: `(86, -5)` and `(128, -10)` — i.e.
  **-0.119 deg per count**.

Note that -0.5 deg/count matches `ecu.h`'s comment that knock retard is
"~0.5 deg per count", and the sign convention (higher count = more negative)
is a *retard*, not an absolute advance. That suggests the active pair
describes a retard quantity and may not be the right scale for
`ECU_DmaData2_t.IgnTiming` at all. **Resolve this against the disassembly
before trusting either.** `IgnAdvanceHi`/`Lo` (`0x246`/`0x247`) is a 16-bit
absolute advance and is probably the better source for a timing display.

### 3.6c KnockRetardInfo[] - the 0.5 deg/count note looks wrong

**Found on the bench 2026-09-05**, first run of the scaled telemetry against
the stimulator: `KnockRetardCyl1/2/3` all read a rock-steady **77.00 degrees**
of retard. That is 154 counts at the 0.5 deg/count `ecu.h` records for
`KnockRetardInfo[]`, and 77 degrees of retard is not a physical quantity.

Note the main `KnockRetard` field reads **0.00 deg** in the same capture, which
is correct for an unstressed stimulated engine - so the scale factor is right
*there* and only questionable for the `KnockRetardInfo[]` array. Either those
three bytes are not per-cylinder retard in the same units, or they are not
retard at all.

Until it is resolved, `CAN_XFORM_RETARD` on those three rows is producing a
confidently wrong number, which is the exact failure mode section 3.2 sets out
to avoid. **Either derive the real encoding from the disassembly or drop those
rows back to `CAN_XFORM_COPY8`** - a raw count is honest, 77 degrees is not.

### 3.7 Frame re-layout

Moving to uniform 16-bit signals costs payload space. `MEDIUM1` is already
full at 8 bytes with seven signals, so it cannot absorb any widening. Proposed
layout (each frame 8 bytes, four 16-bit signals):

| Frame | Period | Contents |
|---|---|---|
| `FAST` (+0) | 20 ms | Rpm, Tps, Pim, InjPw |
| `MEDIUM1` (+1) | 100 ms | Ect, Tha, Battery, AdcLambda |
| `MEDIUM2` (+2) | 100 ms | IgnTiming, IscvDuty, KnockRetard, InjDuty |
| `MEDIUM3` (+5) | 100 ms | KnockRetardCyl1/2/3, LambdaTrim |
| `SLOW` (+3) | 500 ms | NvTrimPim, NvTrimO2, FuelTrim, flags (8-bit) |
| `RAW` (+4) | 50 ms | unchanged — every DMA byte, unscaled |
| `INFO` (+6) | 1000 ms | **new** — protocol version, firmware version, CPU/ECU identity, `CAN_TxDropped`, `CAN_BusOffRecoveries` |

`MEDIUM3` and `INFO` take `+5` and `+6`, still clear of the diag identifiers
at `+10`/`+11`.

**Bus load is a non-issue** and should not constrain the design: per board this
is ~102 frames/s, and at 500 kbit/s a standard 8-byte frame is ~130 bits worst
case with stuffing — about **2.7 % of the bus per board**, ~5.3 % for two,
plus three 1 Hz node heartbeats. There is ample room to add signals.

The `INFO` frame is cheap insurance: it lets a dash node detect a firmware /
DBC mismatch and say so on screen rather than displaying plausible nonsense.
It also surfaces the existing `CAN_TxDropped` and `CAN_BusOffRecoveries`
counters, which `CLAUDE.md` notes are the first thing to read when telemetry
goes quiet.

### 3.8 This is a breaking change

Frame layouts change, so `toyotune.dbc` and `can_monitor.py` must move in the
same commit, and any previously recorded logs decode wrongly against the new
DBC. That is acceptable — telemetry is only deployed on the bench today. Do it
cleanly and in one commit rather than trying to stay compatible.

### 3.9 Testing

- **Host unit tests for `ecu_scale.c`.** Compile it natively, feed known raw
  values, assert the engineering outputs — especially the documented reference
  points (PIM's four table rows, ECT `0xE400` -> 82 degC, THA `0x86` -> 20 degC).
  These curves are the most likely thing to be wrong and the hardest to spot
  on a gauge. The repo already has a Python test-suite precedent in
  `roms/d8x_assembler/tests/`.
- **On the bench:** stimulator running, sweep RPM, confirm `can_monitor.py`
  reports sane engineering units, and cross-check scaled values against the
  `RAW` frame decoded by hand.

---

## 4. Part B — the RP2350 dash node

### 4.1 Hardware

Each node is a **stock Waveshare RP2350-Touch-AMOLED-1.75 dev board on a small
carrier**. The dev board is not automotive — USB-C powered, LiPo charger, no
12 V input, no transient protection — so the carrier supplies exactly what is
missing and nothing more.

**On the dev board (nothing to design):**

| Part | Notes |
|---|---|
| RP2350A | 520 KB SRAM, 16 MB flash, dual M33 / Hazard3, 150 MHz |
| 1.75" round AMOLED, 466×466 | **CO5300** driver over **QSPI** |
| CST9217 touch | I2C + interrupt. **Load-bearing** — swipe left/right selects the page (§4.6) |
| QMI8658 6-axis IMU | Unused, but a lateral-g face is nearly free if wanted |
| AXP2101 PMIC | Feed it regulated 5 V and it handles the board's rails |
| PCF85063 RTC, ES8311 codec, microSD | All unused |
| **Reserved PSRAM pad** | Unpopulated. The escape hatch if §4.1a ever fails |

**On the carrier (designed once, built three times):**

- **12 V automotive input** -> reverse-polarity and load-dump protection ->
  buck to **5 V**, into the dev board's supply.
- **TJA1051T/3** CAN transceiver, with CANH/CANL to the loom connector.
  Spare breakouts are already on hand — see below.
- **One resistor divider** setting the node ID — see §4.5.
- One connector type for the 4-wire daisy chain (12 V, GND, CANH, CANL).

#### Transceiver — reuse the breakouts, with the known modification

Spare **TJA1051T/3 breakouts** are already on hand, so none need ordering.
Two things carry over from the Toyotune board's CAN work and must not be
rediscovered:

- **Remove the on-board AP3602A boost regulator from each breakout and feed
  `VCC` directly with 5 V.** That regulator is a proven failure point — one
  breakout previously thought dead was rescued by exactly this. On the dash
  node it is redundant anyway: the carrier already produces 5 V for the dev
  board, so the boost converter is a fragile part solving a problem that does
  not exist here. Do it by design rather than after a failure.
- **`VIO` to 3.3 V, not 5 V.** The `/3` suffix is why this part was chosen —
  it has a separate logic-supply pin, and **RP2350 GPIOs are not 5 V
  tolerant**, so a plain `TJA1051T` driving `RXD` at 5 V would overstress
  GPIO25. Check the variant marking on each breakout before fitting.

Fit regardless, as on the Toyotune board: a **1 k series resistor in TXD**,
decoupling at the transceiver, and a **TVS** across the bus pins.

With `VIO` off the dev board's 3.3 V and `VCC` off the carrier's 5 V — both
derived from the same 12 V input — each node has a **single ground
reference**, which is the architecture that fixed the earlier transceiver
failure. It also removes the back-powering path where the MCU drives TXD into
an unpowered transceiver.

#### First light on the bench, 2026-09-19

The transceiver works both ways: the node's heartbeat reaches a CANable Pro on
`0x440` once a second, and a time announced from the CANable on `0x450` sets
the dash clock. Three things that bring-up taught, in the order they cost
time:

- **An unterminated bus looks exactly like a broken transceiver.** With no
  resistor across CANH and CANL, nothing pulls the bus back to recessive after
  a dominant bit - it drifts back through the transceivers' input resistance
  against the cable capacitance, microseconds against a 2 us bit - so every
  recessive bit still reads dominant when can2040 reads it back. Every frame
  fails its own readback and is retried at once, about 10,000 times a second,
  and the CANable sees nothing but form and stuff errors. The meter across
  CANH and CANL, power off, read **open**. Measure that first: about 60 R is
  right, 120 R is one end only.
- **The breakout's termination switch did not terminate.** Set to on, it put
  nothing across the bus. Faulty, or labelled the wrong way round - either way
  it cannot be trusted, and the car's bus wants a fitted 120 R at this end.
- **A node with nobody to acknowledge it retries for ever.** That is ordinary
  CAN, not a fault: with the CANable closed and no Toyotune board on the bus,
  the heartbeat is re-sent continuously and each try counts as a failure. The
  counters only mean something while another node is acknowledging.

The console's `can:` line - received, decoded, sent against attempted, parse
errors - is what separated these, and is what M4 will be judged on.

**The breakout's AP3602A regulator is kept on the dash node**, by decision: one
supply and short leads, so none of the ground offset between two 5 V domains
that made it a failure point on the Toyotune board. If the display ever
glitches under bus traffic, the extra load it puts on the 3.3 V rail is the
first thing to look at.

**The loom's ground wire is not incidental.** Dash nodes and the ECU-mounted
Toyotune boards sit metres apart in the car and are grounded at different
points. Carrying GND alongside CANH/CANL in the same 4-wire chain is what
keeps the common-mode offset between them small — comfortably inside the
TJA1051's tolerance, and the reason isolation is not needed here. Do not
"simplify" the loom to two wires.

**GPIO budget and allocation.** The board is an RP2350A, so GPIO0..29 — and
Waveshare's pinout table accounts for every one of them. Everything from
GPIO0 to GPIO24 is committed to on-board hardware, which is where "five
multifunctional GPIO pins" comes from: **GPIO25..29 are what is left.**

| GPIO | Board function | Our use |
|---|---|---|
| **25** | `RXD1` (UART) | **can2040 RX** |
| **26** | `TXD1` (UART), ADC0 | **can2040 TX** |
| **28** | `IMU_INT1`, ADC2 | **Node ID divider** — the QMI8658 is polled over I2C, so its interrupt is never enabled |
| 27 | `RTC_INT`, ADC1 | **Proposed: illumination sense** (§4.10). Usable provided PCF85063 alarms stay disabled |
| 29 | `IMU_INT2` + **`AXP_IRQ`**, ADC3 | **Avoid** — the PMIC genuinely asserts this one |

**Confirmed 2026-09-04: no GNSS module is fitted**, so `GPS_RST`/`RXD1`/`TXD1`
carry no traffic and GPIO25/26 are genuinely free.

**But the pinout image this table was built from has at least two errors,
found 2026-09-12 by reading Waveshare's own code**, which *uses* the pins it
names and is therefore the stronger evidence:

| Pin | Pinout image | Their code |
|---|---|---|
| GPIO8 | `QSPI_SS2` | `DOF_INT1`, the IMU interrupt, configured and read in `QMI8658.c` |
| GPIO24 | `GPS_RST` | `card_detect_gpio`, the microSD card detect, in `hw_config.c` |

The image put `IMU_INT1` on GPIO28, which is why GPIO28 was chosen for the ID
divider with a note that the IMU interrupt would be left disabled. If that
interrupt is really on GPIO8 then **GPIO28 is cleaner than assumed**, but the
table now has two known errors, so **confirm the allocation against the
schematic rather than the image before laying out a carrier.** Nothing in the
C examples references GPIO25 to GPIO29 at all, which is at least consistent
with those five being the free ones.

GPIO25/26 are the cleanest pair: brought out to the UART connector and shared
with no on-board chip. Both are far below 31, so the PIO window constraint is
satisfied trivially.

**They are on the 8-pin 0.1" header, and that header is the whole
connection.** Along the bottom edge of the board, left to right:

| IO29 | IO28 | IO27 | RXD | TXD | 3V3 | GND | VBUS |
|---|---|---|---|---|---|---|---|
| avoid - `AXP_IRQ` | node ID divider | illumination sense (§4.10) | GPIO25, CAN RX | GPIO26, CAN TX | transceiver `VIO` | ground | 5 V, while on USB |

This replaces the plan to mate the transceiver to the SH1.0 4-pin UART plug
and run the ID divider to a separate header. One 8-pin connector carries CAN,
the transceiver's logic supply, a ground, 5 V and the ID pin, so a node is one
connector to the carrier. **Used on the bench 2026-09-19.**

`RXD` is GPIO25 and `TXD` GPIO26 by the chip, not only by the silkscreen: in
the RP2350's function table GPIO25 can only ever be UART1 *receive*, and GPIO26
carries UART1 *transmit* only through its auxiliary function. The transceiver
wires straight through - its `RXD` (pin 4, an output) to the header's `RXD`,
its `TXD` (pin 1, an input) to `TXD`. Breakouts label these from either side,
so check which breakout pin reaches chip pin 4 with a meter rather than
reading the label.

VBUS is USB's 5 V, live only while USB is connected; in the car the carrier
supplies 5 V instead.

**Note GPIO24 is free but not exposed.** With no GNSS fitted it has no
function, but Waveshare's pinout leaves its "Other" column blank — it runs to
the unpopulated GNSS footprint only, not to a header. Treat it as reachable
only by soldering to a pad, not as a sixth free pin.

**GPIO29 is the trap.** It carries the AXP2101 interrupt as well as the second
IMU interrupt, and the PMIC will assert it in normal operation — battery,
charger and button events. Do not treat it as a free pin just because the
docs' "five" arithmetic includes it.

**Reserves, if the allocation ever runs short:**

- **GPIO18..21** — the microSD interface. Four pins, free for the taking if no
  card is ever fitted.
- **GPIO0..5** — the ES8311 audio codec. Unused here, but repurposing them is
  less clean: `GPIO2` is `I2S_DSOUT`, an *output from the codec*, so driving it
  means bus contention unless the codec is held inactive.
- **GPIO24** — `GPS_RST`, free if no GNSS module is fitted (see §6).

**One shared I2C bus.** `GPIO6`/`GPIO7` carry SDA/SCL for **six** devices:
touch, GNSS, RTC, IMU, AXP2101 and the codec. Touch responsiveness is now a
user-facing feature (§4.6), so keep other traffic on that bus sparse — polling
the IMU hard for a lateral-g page is exactly the thing that would make swipes
feel laggy.

**`GPIO17` is `LCD_TE`** — the panel's tear-effect output. Worth using: it
lets updates be synchronised to the panel refresh, which is what keeps a
sweeping needle from tearing.

**`GPIO8` is `QSPI_SS2`**, the chip select earmarked for the reserved PSRAM
pad. So populating PSRAM (§4.1a) costs none of the five free pins.

Nothing else needs a pin. In particular **display brightness is a CO5300
command, not a PWM line**, so dash-illumination dimming costs no GPIO — it is
a software write.

**The CAN pins must be PIO-reachable, and both must sit in the same PIO
window.** can2040 is software CAN running on a PIO block, so its TX and RX
pins are not free choices. On RP2350 a PIO instance sees a 32-pin window —
either GPIO 0..31 or GPIO 16..47 — and can2040's own documentation is explicit
that `gpio_rx` and `gpio_tx` must both fall inside one of them.

**On this board that constraint cannot bite.** The docs specify an
**RP2350A**, the QFN-60 part, which only has GPIO 0..29 — every pin on it is
inside the 0..31 window, so any two exposed GPIOs are valid can2040 pins. Worth
recording anyway, because it stops being free the moment anyone substitutes an
RP2350B board with pins above 31 broken out.

**PIO block budget, answered and tighter than assumed.** Waveshare's own
C/C++ documentation states the panel is driven by **PIO emulated QSPI**, not
by a hardware SPI or the QMI peripheral. Their source names the block:
`qspi_pio.c` sets `.pio = pio0`, so can2040 must take PIO1 or PIO2, and
`can_link.h` already selects PIO1. So of the three PIO blocks:

| Block | Claimed by |
|---|---|
| PIO0 | CO5300 QSPI panel (vendor driver) |
| PIO1 | can2040 - it needs a whole block to itself |
| PIO2 | ES8311 audio - the warning beeps, section 4.12 |

**All three blocks are now claimed.** This table used to record the third as
spare on the grounds that the codec was unused; it is used, for the warning
sounds, and it needs two state machines of PIO2 (master clock, and the data
line) out of that block's four. So there is no longer a free block, and
anything else wanting PIO has to share PIO2 - which is fine, it has two state
machines and ten instruction slots left, but it is no longer free real
estate.

**This raises the stakes on M4.** The panel driver DMAs colour data in large
bursts while can2040 needs low interrupt latency on the other core. Bus
contention between a big display DMA and the core servicing the CAN IRQ is
exactly the failure this project must not discover in the car — see §4.2.

### 4.1a Memory and rendering strategy

466×466 at 16 bpp is **424 KB**, against 520 KB of SRAM and **no PSRAM
fitted**. Holding a whole frame would leave ~96 KB for stacks, can2040, LVGL
internals and the application — too tight to be comfortable, and a double
buffer (848 KB) is impossible.

**This is not a problem, because a full framebuffer is not how LVGL drives a
panel.** Use partial rendering: a draw buffer of roughly 1/10 of the screen,
double-buffered, is about **85 KB**, leaving ~435 KB free. LVGL draws dirty
rectangles into that buffer and DMAs them out over QSPI.

That suits gauges well — a sweeping needle dirties a modest band, not the
whole face. What it costs is cheap full-screen effects, which this design does
not want anyway.

Bandwidth is not a concern on QSPI: a full frame is 3.47 Mbit, so at ~80 MHz
across four lanes a *complete* refresh is ~11 ms. Partial updates are far
smaller.

**The vendor example overflows its draw buffer, confirmed in source
2026-09-12.** Waveshare's `05_LVGL` ships **LVGL 8.1.0** and allocates a
full-screen buffer:

```c
#define LV_COLOR_DEPTH 16                                 /* lv_color_t = 2 bytes */
buf0 = (lv_color_t *)malloc(DISP_HOR_RES * DISP_VER_RES); /* 217,156 BYTES  */
lv_disp_draw_buf_init(&disp_buf, buf0, NULL,
                      DISP_HOR_RES * DISP_VER_RES);        /* 217,156 PIXELS */
```

The allocation is in bytes and the size handed to LVGL is in pixels. At 16 bit
colour those differ by two, so LVGL believes it has 434,312 bytes in a
217,156 byte allocation: **a 212 KB heap overflow** the moment it redraws
enough of the screen at once. It survives their demo because partial refreshes
never reach the far half. Do not copy it; the vendored copy is kept as
`firmware/vendor/LVGL_example.c.reference` for reading only.

Doubling the malloc would not help either. 424 KB of a 520 KB SRAM leaves
nothing for LVGL's own 32 KB heap, can2040 and the application, which is why
partial buffers are the arrangement here.

**The PSRAM escape hatch does not exist on this board, measured 2026-09-12.**
`firmware/test/psram_probe.c` drives Waveshare's own detection code against
QSPI chip select 1 (pad 47) and nothing answers the JEDEC Read ID with AP
Memory's KGD byte:

```
no PSRAM detected on QSPI CS1 (pad 47)
```

The pad really is empty, as this section originally assumed. So **partial
rendering is a requirement rather than a preference, and there is no fallback
if it proves too slow**, which raises the stakes on measuring it at M2.

Populating the footprint stays possible: it is an 8 pin SOIC AP Memory
APS6404L class part on the same JEDEC serial memory footprint as the flash
chip beside it, and the vendor driver detects and sizes it at runtime. That is
now a hardware modification rather than something the board already offers. If
it is ever fitted, note its two constraints: 109 MHz maximum clock, and an
**8 us ceiling on chip select assertion** for refresh, so long DMA bursts to
PSRAM have to be broken up.

**Toolchain baseline.** The vendor examples are pico-sdk + CMake under the
official VSCode extension — the same shape as the existing SAMC21 tree — with
LVGL 8.1 vendored under `lib/lvgl`. Staying on 8.1 initially is the pragmatic
choice because the panel and touch glue is written against it; LVGL 9 is
current and a later port is a deliberate piece of work, not a free upgrade.
Useful starting examples are `01_GUI` (panel + drawing) and `05_LVGL`.

### 4.2 Core split

| Core | Responsibility |
|---|---|
| **Core 0** | can2040 + its PIO IRQ, frame decode, signal store |
| **Core 1** | LVGL, rendering, panel DMA |

can2040 is explicitly built for this: its functions are not reentrant across
cores, but `can2040_pio_irq_handler()` is reentrant-safe with respect to other
can2040 calls **on the same core**, and instances on different cores need no
synchronisation.

**The critical constraint:** can2040 is bit-timing sensitive and needs low
interrupt latency. Its documentation is explicit that can2040 and anything
running at higher priority should be **placed in SRAM, not XIP flash** — a
flash load costs ~320 ns on RP2040 at 125 MHz, and a cache miss inside the CAN
IRQ corrupts a bit. Design this in from the start:

- Mark can2040 and its IRQ handler for SRAM placement.
- Keep the display DMA and LVGL entirely on core 1.
- Keep core 0's other interrupt work minimal and short.
- Give can2040 a different PIO block from the panel driver (§4.1).
- **Watch DMA contention.** The panel driver moves colour data by DMA in
  large bursts. That competes for bus bandwidth with the core servicing the
  CAN IRQ, and is the most likely mechanism by which M4 fails. Consider
  capping the display DMA burst size if it does.

500 kbit/s is well inside can2040's envelope (it is rated to 1 Mbit/s), so
this should be clean — but **verify it under full render load** before
committing to a PCB (milestone M4). If it is dirty, fall back to an MCP2518FD
on SPI: a hardware controller, no core burn, no timing sensitivity. That is a
footprint decision, so it must be settled pre-layout.

### 4.2a Display DMA vs the CAN IRQ — the levers

**can2040 uses no DMA.** It takes one PIO block (all four state machines —
`sync`, `rx`, `match`, `tx`) and services the FIFOs *from the interrupt
handler on the ARM core*. So there is no can2040 DMA channel whose priority
could be raised, and the DMA controller's per-channel `HIGH_PRIORITY` bit in
`CHx_CTRL_TRIG` is no help here: it arbitrates **between DMA channels**, and
the display is effectively the only significant DMA user.

The contention is therefore **display DMA (a bus master) against the CPU core
running the CAN IRQ (another bus master)**, competing on the AHB crossbar.
Three levers, in the order they should be tried.

**1. SRAM bank placement — try first, costs nothing at runtime.**
Contention only occurs when two masters hit the *same* SRAM bank in the same
cycle. Main SRAM is striped across banks, but pico-sdk exposes non-striped
scratch regions (`__scratch_x`, `__scratch_y`). Putting can2040's hot code and
data there keeps it out of the banks the display DMA is streaming from. This
also satisfies the SRAM-residency requirement from §4.2 — pair it with
`__not_in_flash_func` so nothing in the CAN path can take an XIP cache miss.

**2. Cap the display DMA burst size.** Shorter transfers mean a shorter
worst-case stall for the core. Partial rendering (§4.1a) already helps a great
deal here, because dirty-rectangle updates move far less data than full-frame
blits — another reason not to inherit the vendor example's full-screen buffer.

**3. `BUSCTRL->BUS_PRIORITY` — the real priority knob, and the blunt one.**
This sets AHB arbitration priority per bus master:

| Field | Bit | Meaning |
|---|---|---|
| `PROC0` | 0 | 0 = low, 1 = high |
| `PROC1` | 4 | 0 = low, 1 = high |
| `DMA_R` | 8 | 0 = low, 1 = high |
| `DMA_W` | 12 | 0 = low, 1 = high |

`BUS_PRIORITY_ACK` is read-only and reads 1 once the arbiters have registered
the new levels. Note the fix is the *inverse* of raising the DMA: you promote
the core running can2040 and leave the DMA low — with can2040 on core 0,
`bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_PROC0_BITS;`

**Use this last, and measure both sides.** It is a strict priority, not a
weighting, so whatever you promote can starve everything else. pico-sdk issue
[#2123](https://github.com/raspberrypi/pico-sdk/issues/2123) reports exactly
this backfiring on RP2350: code that had been fine on RP2040 set
`BUSCTRL_BUS_PRIORITY_PROC1_BITS` and left the *other* core sluggish enough to
break WiFi and slow SD card reads. Trading CAN bit errors for a stuttering
gauge is not a win. M4 must measure render throughput as well as CAN error
counts.

### 4.3 Cross-core data sharing

Core 0 writes decoded signals; core 1 reads them while rendering. Use a
**seqlock** (single writer, sequence counter incremented before and after each
update; the reader retries if the counter changed or is odd). This is the
right pattern here — it needs no locks, never blocks the writer, and the
renderer simply retries on the rare collision.

Store per signal: **value, and the timestamp of last update.**

### 4.4 Staleness

With independent nodes, a gauge silently displaying a frozen value is worse
than one that is obviously dead. Track age per signal against its frame period
and degrade explicitly — grey out, or replace with dashes — at roughly 3x the
expected period. Build this in from the start; it is painful to retrofit.

### 4.5 Node identity — one ADC pin, not two straps

Node identity must not be a build-time `#define`. If it is, "build one and
replicate twice" produces *three binaries*, and flashing the wrong one becomes
a real mistake — exactly the hazard `CLAUDE.md` already flags for
`TOYOTUNE_CPU` on the Toyotune boards, where it notes a strap would have made
one binary genuinely universal. This is where that pays off.

**Encode the ID as a resistor divider on a single ADC input.** The board
exposes 12-bit ADCs, so one pin distinguishes far more than the four IDs
needed, with generous margin between levels. Firmware reads it once at boot
and indexes the face table (§4.6).

Two reasons this beats the two-digital-strap scheme originally planned here:

1. **Pin budget.** Only five GPIOs are free (§4.1). One pin instead of two
   leaves two spare rather than one.
2. **It sidesteps erratum E9.** RP2350-E9: a GPIO configured as a *digital
   input with the internal pull-down enabled* can latch at ~2.1-2.2 V after
   being driven high, sourcing ~120 uA, and so reads high when it should read
   low. An ADC pin in analogue mode has its digital input buffer disabled, so
   the erratum does not apply at all — no pull-up-and-short-to-ground
   workaround needed.

Keep the divider impedance low (a few kOhm) so the ADC sees a stiff source,
and space the ID levels widely across the range rather than packing them.

### 4.6 Pages as data, selected by swipe

**Touch is not spare hardware — it is the interaction model.** Swiping left
and right on a node cycles it through a shared list of pages. That changes the
structure from "three fixed faces" to **three independently steerable windows
onto a larger set of pages**, which is a real capability gain: the page list
can hold six or eight configurations and the driver picks any three at a time.

The node ID (§4.5) therefore selects the **startup page, not the only page**.
It is still needed — for the heartbeat identity and for a sane power-on
layout — but it no longer welds a node to one gauge.

Structure it as a page list, each page a table of elements, mirroring the
table-driven style of `can_telemetry.c`:

```c
typedef struct {
    WidgetType_t Type;      /* dial, arc, rolling graph, numeric, bargraph */
    SignalId_t   Signal;
    int32_t      Min, Max;
    uint8_t      X, Y, W, H;
} FaceElement_t;

typedef struct {
    const char          *Name;
    const FaceElement_t *Elements;
    uint8_t              ElementCount;
} FacePage_t;

static const FacePage_t Pages[];       /* the shared list, same on every node */
static const uint8_t    StartupPage[]; /* indexed by node ID */
```

Adding a gauge is a row; adding a page is a table; adding a node is a startup
index. **If you find yourself writing node-2-specific code, something has gone
wrong.**

**As built, not through LVGL.** This paragraph used to say LVGL's gesture
detection and tileview would give the page transition for free. LVGL has since
left the firmware entirely (`RENDERER_PLAN.md`), and the swipe is our own: the
page follows the finger across two live surfaces, with momentum on release -
see `THE SWIPE` in `main.c`.

#### Two views per page - the vertical swipe (built 2026-09-18)

Swiping **left and right moves between pages; swiping up and down flips a
page between its two views**:

| Page | View 0 | View 1 |
|---|---|---|
| Engine speed | tachometer | RPM trace |
| Boost / AFR | boost over mixture, half dials | both, as a trace |
| AFR / EGT | mixture over exhaust temperature | both, as a trace |
| Air temps | intake over manifold air temperature | both, as a trace |
| G-force | friction circle | lateral and longitudinal g, as a trace |
| Clock | analogue, MR2 style | the time in large figures, seconds beneath |
| Warning | the takeover - one view only | |

The decisions behind it:

- **The second view is part of the page, not a page of its own.**
  `FacePage_t` carries `AltElements`, so a reading has one place in the list
  however it is being shown. That is why the standalone Boost trace page went
  away: it is the Boost / AFR page flipped, one swipe from the needles rather
  than four pages along.
- **A graph view shows exactly what its gauges show** - the same signals,
  ranges, scale labels and formatting - so a reading cannot change meaning
  across a swipe. The host tests enforce it.
- **Each page remembers its own view**, so swiping away from a trace and back
  finds the trace again. Every page opens on view 0 at power-on; the view is
  not persisted, and should be once page persistence (below) is written.
- **Only the view on the glass is drawn.** The other is built into the spare
  surface when a vertical swipe starts, exactly as the neighbouring page is for
  a sideways one. What runs all the time is the graph history's sampling -
  one reading per trace every 33 ms, no drawing - so a trace already holds its
  last ten seconds when it is flipped to.
- **The g-force trace reads the accelerometer through the UI model's local
  readings** (`UiModel_SetLocal`), because it is read on core 1 and the signal
  store has a single writer on core 0.
- **Faces are pre-rendered per view**: twelve at 195 KB each, about 2.4 MB of
  the 16 MB flash. Flash is not the constraint; SRAM is, and views cost it
  nothing.
- The console's `f` flips the view, for the bench.

The fault takeover still outranks everything: no swipe of either kind starts
while it stands, and one arriving mid-swipe ends it.

#### The page list

As built, easy to change now it is data: **Engine speed, Boost / AFR,
AFR / EGT, Air temps, G-force, Clock**, each with the second view above, and
the warning takeover. Nodes start on Engine speed, Boost / AFR, G-force and
AFR / EGT by identity.

Split faces - two half dials sharing one centre - are the pattern for pairs
that belong together: boost with mixture, mixture with exhaust temperature,
intake air with the charge in the manifold. A scale that goes below zero must
say `PAGES_NO_BAND` for no red band, because 0 is then a real place on the
dial rather than "none".

Still in the original plan and not built:

1. **RPM** — shift ring and limiter flags on the outer arc. (The dial is
   built.)
2. **Boost** — peak-hold numeric. (The dial and the trace are built.)
3. **Health** — ECT, battery, lambda, injector duty as a compact cluster.
4. **Knock** — per-cylinder retard bars plus ignition timing.
5. **Fuel** — injector duty, pulse width, **wideband lambda** (from the
   14Point7, §4.9) with the ECU's narrowband `AdcLambda` and `LambdaTrim`
   beside it, and the learned trims.
6. ~~**Lateral g**~~ — built, as the G-force page.

#### Interaction details worth settling early

- **Require a deliberate swipe**, with distance and velocity thresholds — not
  a tap. Capacitive touch does not false-trigger on vibration, but condensation
  and a wet fingertip will, and a gauge that changes page over a bump is worse
  than no touch at all. A tap should do nothing at all, or at most acknowledge
  a warning.
- **Persist the selection.** Resetting to the startup page on every ignition
  cycle would make the feature useless. Write the page index to flash **only
  on change** — the board has 16 MB and changes are rare, so wear is a
  non-issue. (The PCF85063 RTC also has a RAM byte, but flash is simpler and
  does not depend on how the RTC's backup supply is wired.)
- **The warning takeover outranks the selection.** When an `ErrorFlags` /
  `LimiterFlags` bit sets or knock retard crosses threshold, the node switches
  to the warning face regardless of the selected page, and returns afterwards.
  A driver must not be able to swipe away from a fault.
- **Independent or coordinated?** See §6 — this is the one open decision the
  touch feature creates.

**Physical caveat:** capacitive touch works through a thin non-conductive
cover but degrades badly across an air gap or a thick lens. If the panels end
up recessed in pods, the touch surface has to stay reachable, which constrains
the bezel design. Settle this alongside the outline dimensions at M0.

### 4.7 Node heartbeat

Each node transmits a 1 Hz heartbeat: node ID, uptime, CAN error counters,
frames-decoded count, **and the page it is currently showing**. This
distinguishes "gauge 2 is dead" from "gauge 2's panel is dead". Identifiers
`0x440`, `0x441`, `0x442` — clear of the telemetry blocks at `0x400`/`0x420`
and of OBD2's `0x7DF`/`0x7E0`/`0x7E8`.

Carrying the current page costs one byte and is what makes coordinated paging
possible later without a new frame — a node can see what its neighbours are
showing and skip a duplicate. Include the byte from the start even if paging
stays independent.

### 4.9 The 14Point7 wideband — a third transmitter on the bus

A **14Point7 wideband lambda controller** will also sit on this bus - a
**Spartan 3**, which with a K-type thermocouple fitted also reports **exhaust
gas temperature**. It is the first participant that is neither a Toyotune
board nor a dash node, and that has three consequences.

**Decoding it is still to do.** Both of its readings already have a place on
the dash - `SIGNAL_AFR` and `SIGNAL_EGT` in `signals.h`, shown on the AFR / EGT
page (§4.6) - but nothing writes them yet except the bench simulator. The
decoder waits for the frames to be logged, below. `SIGNAL_EGT` is given the
MEDIUM period rather than FAST on the guess that a thermocouple is reported
slowly; confirm the real rate from the log, or the reading will be called
stale between two good frames.

**1. It is a strictly better lambda source than the ECU's, and the pages
should use it.** `AdcLambda` in the ECU telemetry is a *narrowband* sensor
voltage — it switches around stoichiometric and tells you essentially nothing
about actual mixture either side of it. The wideband gives real lambda across
the range.

But **keep both on display, because the difference is the diagnostic.**
`AdcLambda` is what the ECU *believes*; the wideband is what is actually
happening. Divergence between them is exactly how you see closed-loop control
failing, a lazy sensor, or a fuelling problem the ECU is not reacting to. The
Fuel page (§4.6) should show wideband lambda as the primary value with the
ECU's narrowband state and `LambdaTrim` beside it.

**2. Identifier collision is the real hazard, and it is cheap to settle.**
The device is built to feed Megasquirt 3, Haltech, Link, Adaptronic, HP
Tuners, MaxxECU and others, so its identifiers are whatever those ECUs expect
— not chosen to avoid ours. Toyotune already occupies `0x400`-`0x404` and
`0x40A`/`0x40B` (CPU1), `0x420`-`0x424` and `0x42A`/`0x42B` (CPU2), with
`0x440`-`0x442` planned for dash heartbeats.

Do **not** take the manual's word for it alone: put the wideband on the bench
bus and log it. `can_monitor.py` or a raw `candump` answers this empirically
in minutes, and also catches the case where the device transmits more than the
one frame its manual documents. If there is a clash, whichever side is
configurable moves — the Toyotune base identifier is a `config.h` constant
(`TOYOTUNE_CAN_ID_TELEMETRY_BASE`), so ours is the easier one to shift.

**3. Bit rate — settled.** Confirmed 2026-09-04 that the wideband's bit rate
is **configurable**, so set it to **500 kbit/s** and it shares this bus
directly. No second bus, no gateway. This was the only genuine go/no-go in
integrating it, and it is closed.

That also raises the odds on the identifier question above. A device whose bit
rate is configurable, and which offers a protocol selection per target ECU
(Megasquirt, Haltech, Link, ...), very likely changes its identifier set with
that selection — so a clash may be avoidable simply by choosing a different
protocol profile, without touching `TOYOTUNE_CAN_ID_TELEMETRY_BASE` at all.
Log the bus under each candidate profile and pick one that lands clear of
`0x400`-`0x404`, `0x420`-`0x424`, `0x40A`/`0x40B`, `0x42A`/`0x42B` and
`0x440`-`0x442`.

Added bus load is negligible: a lambda controller broadcasts one or two frames
at tens of hertz, well under 1% against the ~5.3% the two Toyotune boards use.

**Once decoded, add it to `toyotune.dbc` as its own sender node.** Its output
is already in engineering units, so none of the §3 scaling work applies — this
is straight transcription from the manual, verified against the logged frames.
Keeping it in the same DBC means `can_monitor.py`, the dash nodes and any
logger all decode it from one source of truth.

**Worth noting for later:** a true wideband on the bus is the prerequisite for
ever closing the ECU's fuel loop against real lambda instead of the narrowband
it was designed around. Out of scope here, but a reason to make sure this data
is cleanly decoded and logged rather than merely displayed.

### 4.8 Physical installation — the single-DIN aperture

The cluster mounts in a **single-DIN radio aperture: 180 x 50 mm** (ISO 7736).
Width is not a constraint; **height is, and it is the dimension that decides
the panel size.**

| Panel | Active dia. | Vertical margin each side | Three across | Gap between/around |
|---|---|---|---|---|
| 1.43" | 36.3 mm | **+6.8 mm** — comfortable | 109.0 mm of 180 | 17.8 mm |
| 1.75" | 44.4 mm | **+2.8 mm** — tight | 133.3 mm of 180 | 11.7 mm |
| 2.00" *(the original goal)* | 50.8 mm | **−0.4 mm — will not fit** | 152.4 mm of 180 | 6.9 mm |

**The original "roughly 2 inch" target was never compatible with this
aperture.** At 50.8 mm a 2" panel is physically taller than the 50 mm opening.
The constraint settles that: something smaller was always required.

**The module outline does not have to fit *through* the aperture.** Only the
active circle needs to fall inside the 50 mm visible band; the PCB can be
taller and sit behind the dash face, hidden by the aperture edges. Depth is
not a problem either — a DIN slot offers ~160 mm and a module plus carrier
needs perhaps 30 mm. So the real question is not "is the board under 50 mm
tall" but **"can it be mounted with its glass presenting through a 180 x 50
fascia".**

**Construction:** a custom **180 x 50 mm fascia plate** with three circular
apertures, modules mounted to it from behind, the whole assembly held by
standard single-DIN cage brackets. 3D printed or laser cut.

**The fascia must not recess the glass.** Touch is load-bearing now (§4.6),
and capacitive sensing degrades badly across an air gap or a thick lip. Mount
the glass **flush with or proud of** the fascia front, not sunk behind a
cutout — otherwise swipe paging is compromised by the mechanical design rather
than by anything in firmware.

**Which panel goes in the car is deferred to M7, and this inverts the earlier
recommendation.** The 1.75" was chosen for being closest to the ~2" target;
under a 50 mm aperture that advantage becomes a 2.8 mm margin, against 6.8 mm
for the 1.43". Both are **466 x 466**, so *every line of firmware transfers
between them unchanged* — the ordered 1.75" board is the development target
regardless, and nothing is wasted if the car ends up with 1.43" panels.

Aesthetically the 1.75" fills the aperture better (11.7 mm gaps against
17.8 mm, which reads sparse). Decide by measuring the real module, not from
the datasheet. If switching to the 1.43", confirm it is the **V2.0 QSPI**
revision and check whether its driver IC is also CO5300 — the V1.0 is plain
SPI, and a different controller would mean new panel glue even though the
resolution matches.

---

### 4.10 Night-time dimming

**Design item, not built.** An AMOLED set bright enough to read against a sunlit
windscreen is a glare source at night, and the dash nodes sit beside the car's
own instruments, which the driver dims with the panel-light wheel. The nodes
must follow.

The output side is solved: brightness is a CO5300 register write (§4.1), and
`Panel_SetBrightness()` already takes a percentage. What is missing is the
**input** - something that says it is dark, and how dark the driver wants it.

**Recommended: sense the car's instrument-illumination feed.** The circuit that
lights the SW20's own gauges when the side lights are on, downstream of the
dimmer wheel, goes through a divider to **GPIO27 / ADC1** on each node:

- **no voltage** - lights off - day brightness;
- **voltage present** - lights on - night brightness, scaled by the level, so
  the wheel dims the new gauges together with the old ones.

One wire per node, all three in the same binnacle and harness, and no pin is
spent that is not already spare (§4.1 GPIO table). It follows the driver's own
choice rather than guessing at it.

**Two things to measure on the car before designing the input:**

1. **How the SW20 dimmer controls the lamps.** A series rheostat gives a
   steady, lower voltage, which a divider and the ADC read directly. A pulsed
   (PWM) dimmer gives full-voltage pulses at some duty, which a divider alone
   would read as noise - it needs an RC filter ahead of the ADC, or the duty
   measured on a digital input instead. Put a scope on the feed with the wheel
   at both ends and in the middle.
2. **The voltage range at the feed**, lights on, engine running, wheel at both
   ends. A 12 V system runs to about 14.5 V charging and spikes beyond it, so
   the divider needs margin, and the input a clamp and series resistance -
   the same protection the node's other car-facing lines get.

**Alternatives considered**

- *An ambient light sensor per node.* Automatic, no car wiring - but the board
  has none, it would need a view of the cabin from inside the binnacle, and it
  ignores the driver's dimmer: the new gauges would disagree with the old ones.
- *Over CAN.* One node, or the Toyotune board, reads the feed and broadcasts a
  level; the heartbeat (§4.7) could carry it. That moves the wiring rather than
  removing it, and makes night brightness depend on the bus being up. The ECU
  does not appear to know the light state itself - nothing in the
  reverse-engineering notes mentions a lighting input - so it cannot be taken
  from the existing telemetry. Worth keeping in mind if one node turns out to
  be much easier to wire than the other two.

**Firmware, whichever source**

- A **day level** and a **night level**, the night level scaled by the sensed
  dimmer position when there is one.
- A **floor**, so the wheel turned fully down never leaves a gauge unreadable -
  and a warning takeover (§4.6) may want to override the floor upward.
- A **fade** between levels over about half a second rather than a step, which
  reads as a fault at night.
- **Hysteresis** on the on/off decision and **filtering** on the level, so a
  noisy reading, or the voltage dip of the starter, does not make the display
  flicker.
- The **cold-start** level: the node boots before any reading settles, so it
  starts at the last level used or at night level - never at full - and fades
  from there.

### 4.11 Updating over CAN - Katapult

**Design item, not built.** Once the gauges are in the dashboard, flashing them
means getting at a USB socket behind the dash - so the intent is to update them
over the CAN bus they are already on, with
[Katapult](https://github.com/Arksine/katapult), Kevin O'Connor's bootloader.
It is a natural fit: its RP2040 port uses can2040, the same software CAN this
firmware runs, by the same author.

**Checked 2026-09-17: it supports the RP2350.** Its README still lists only
"lpc176x, stm32 and rp2040", but `src/rp2040/Kconfig` offers a processor choice
of rp2040 or rp2350 (150 MHz), with CAN and with RX and TX pins configurable
anywhere in GPIO 0..29 - which covers this board's GPIO25/26 (section 4.1).
Read the Kconfig rather than the README.

**What has to be decided, and most of it before the dash is closed up**

1. **Flash layout.** The bootloader takes the start of flash and the
   application moves to an offset, which changes the linker script and the UF2.
   There is room: the image is about 440 KB. On RP2350 the boot ROM wants an
   image definition block in the binary it starts, so the hand-off from
   bootloader to application is the part to prove on the bench before trusting
   it in the car.
2. **Identifiers.** Katapult has its own CAN identifiers, and they must land
   clear of Toyotune's `0x400`-`0x404` and `0x420`-`0x424`, `0x40A`/`0x40B` and
   `0x42A`/`0x42B`, the `0x440`-`0x442` dash heartbeats (section 4.7) and
   whatever the 14Point7 turns out to use (section 4.9, still open). Read
   `docs/protocol.md` for what it uses, then log the bench bus to confirm -
   the same discipline section 4.9 asks for.
3. **How the bootloader is entered.** Katapult offers three ways: a double
   press of the reset button, a GPIO, or a CAN request from `flashtool.py -r`.
   **In a dashboard only the CAN request is any use**, and it is the
   APPLICATION that has to honour it: a command on a Toyotune-range identifier
   that stores the bootloader's magic value and reboots. So:
   **never flash a car node with firmware that cannot be asked to reboot into
   the bootloader** - the only way back in would be the reset button or USB,
   which means taking the gauge out.
4. **Which node.** Katapult addresses a board by its own unique identifier, so
   three identical nodes can be flashed one at a time without ambiguity. That
   is separate from the resistor-divider node identity (section 4.5), which
   decides what a node DISPLAYS; do not conflate them. Worth printing both in
   the console status line so a flash can be aimed with confidence.
5. **The failure that matters.** A flash interrupted half way must leave the
   bootloader intact and the node still reachable over CAN - otherwise the
   recovery is the thing this exists to avoid. Bench-test an interrupted flash
   deliberately, with the power pulled mid-write, before relying on it.
6. **Bus and display while flashing.** The application is not running, so the
   panel holds whatever was last on it and the node sends no telemetry. A
   flash should therefore be a stationary-car operation, and the other nodes on
   the bus will see one node's heartbeat stop - which their own fault handling
   must not turn into a warning takeover.
7. **can2040 in both.** The bootloader and the application each run their own
   copy on the same PIO block, never at the same time. No conflict, but the
   bootloader's bit timing has to match the bus - 500 kbit/s (section 4.9).

**Worth doing early rather than late:** the flash layout and the
enter-bootloader command change the application build, so bringing them in
while the board is still on a bench USB cable costs nothing, and leaves the
cable as the fallback while the CAN path is proven.

### 4.12 Warning sounds

**Working on the board, 2026-09-18.** `src/tone.c`, `src/warn.c`,
`src/audio.c` and `src/audio_i2s.pio`, with 872 host checks. All four patterns
play through the fitted speaker, tell apart by ear, and are loud enough at the
default level. The codec answers with its own chip ID (`0x8311`) and the
stream has run with no late buffer.

**Why sound at all.** Every warning this node can show is a warning the driver
is not looking at, because they are looking at the road. The three that matter
are the three where a second of delay costs an engine: the limiter, boost past
what the engine is mapped for, and a lean mixture under load.

**The hardware was already fitted and was written off in section 4.1.** The
board carries an **ES8311** codec on GPIO0..5 with an **MX1.25 2-pin speaker
connector**, which that section lists as "unused". It is used now. Neither
Waveshare's wiki nor the product page publishes the I2S pin assignment or the
codec's address, so both came from **their own `examples/C/02_ES8311`**:

| | |
|---|---|
| I2C | 0x18, on the GPIO6/7 bus the touch panel, IMU and RTC already share |
| GPIO1 | data, MCU to codec |
| GPIO2 | data, codec to MCU - the microphone, unused here |
| GPIO3 | master clock, MCU to codec, 6.144 MHz |
| GPIO4, GPIO5 | bit clock and word clock, **codec to MCU** |
| GPIO0 | **the speaker amplifier's enable** (`PA_CTRL`), high = on |

**The codec is the I2S master, which is the fact the design turns on.** Given
a master clock it generates its own bit and word clocks, so the RP2350 only
has to produce MCLK and shift one bit per edge - both a few PIO instructions,
and neither a peripheral this chip has. That is why the data program is a pure
slave with no clocking of its own to get wrong. Sample rate is **24 kHz with
a 6.144 MHz MCLK**, the one combination Waveshare run on this board and a row
the ES8311's coefficient table has; `audio.c` writes that row out rather than
carrying their hundred-row table and searching it.

**The stream never stops.** Starting the DMA when a warning begins and
stopping it after would stall the state machine on `pull` with the data line
wherever the last bit left it, which the codec turns into a held DC level in
the speaker. So it runs from boot to power-off and silence is a buffer of
zeros: a fixed 96 KB/s of DMA, two 21 ms buffers chained to one another, each
refilled in the interrupt the other one's completion raises.

**All of it is on core 1**, twice over deliberately: the codec is on an I2C
bus that has one owner, and core 0 is where can2040 wants its interrupt
latency. The refill interrupt is at the **lowest** priority so the panel's
tearing-effect interrupt always wins - a late frame is visible and a late beep
is not.

**Which node sounds what.** A node sounds a warning when the face it is
showing displays the reading the warning is about: the limiter on whichever
node shows the tachometer, boost and mixture on whichever shows boost or AFR
(the needle page or the trace, either counts). Sound and sight stay together,
and three speakers cannot announce one event three times a fraction apart. The
exception is the fault takeover, which puts the same page on every node at
once - there **node 0** sounds it and the others stay quiet. That is a guess
about a car that has not been wired; if the speaker ends up elsewhere it is
one constant.

**The patterns are rhythms first and pitches second**, because what tells a
driver which warning is sounding is the pattern of pips, not the note:

| Warning | Sound | Condition |
|---|---|---|
| Fault | two-tone 880/1175 Hz, no gap | whatever raises the takeover page |
| Rev limit | three 60 ms pips at 2600 Hz | 6800 rpm, off at 6500 |
| Overboost | two 120 ms pips at 1800 Hz | 1.05 bar, off at 0.95 |
| Lean | one 400 ms note at 700 Hz | leaner than 13.0:1 **above 0.3 bar** |

Four things about those that are decisions rather than numbers:

- **The lean warning is gated on load, and that gate is the whole reason it is
  usable.** A cruise at 16:1 is the ECU doing its job, so an ungated mixture
  window would beep down every motorway. Rich is deliberately not sounded at
  all: it costs power, not pistons.
- **Every threshold has hysteresis and a 400 ms minimum hold.** Without the
  first a needle resting on a threshold chatters; without the second a spike
  that crosses for one frame gives a click instead of a recognisable sound.
  The hold is not an arbitrary number - it is longer than the sounding part of
  every pattern, which a host test enforces, so a momentary spike is always
  heard as the whole warning rather than as a fragment of one.
- **A stale reading never warns.** If the link drops every condition goes
  quiet, because a warning derived from a value that is no longer arriving is
  a statement about the past presented as the present. It does finish the
  current hold rather than cutting mid-pip.
- **The thresholds are not the face's red band.** They are close on purpose
  and must be able to move apart: a band starting at 1.0 bar looks right
  painted on a dial, while the tone wants a little more so a needle resting on
  the line does not chirp.

**Proving it on a bench.** Nothing else here can prove a speaker works without
an engine to make it go off, so the console sounds each pattern: `1` fault,
`2` rev, `3` boost, `4` lean, `0` silence, each for three seconds, and `Vnn`
sets the codec volume. A console sound outranks a real warning but only for
those three seconds - a bench test that could mask a fault indefinitely would
be the wrong trade. The status line reports the codec's chip ID, the volume,
buffers sent, late refills, and how many times each warning has fired.

**Two things the first flash got wrong, both silent in the same way** - chip
ID correct, stream running, not a sound:

- **GPIO0 enables the speaker amplifier.** The codec only drives a line-level
  output; the amplifier behind it is off until `PA_CTRL` goes high. Waveshare
  set it in `DEV_GPIO_Init()`, which a search of their example for the codec's
  own pins does not turn up - that is how it was missed. If audio ever goes
  quiet with everything else healthy, check this pin first.
- **The volume register is logarithmic, not a percentage.** It is 0.5 dB a
  step with 0 dB at 0xBF, which on Waveshare's 0-100 scale is 75. The first
  default of 55 was about -26 dB, on top of a tone generated at a third of
  full scale. Now the tone is generated just short of full scale and the codec
  sits at exactly 0 dB, which is as loud as it goes cleanly: above 75 is
  digital gain that clips the sine. `Vnn` on the console turns it down, about
  1.3 dB a point.

**Open:**

- **The fault chime sounds on node 0 by assumption.** Once the cars wiring is
  known, either move it or - if every node gets a speaker - give the nodes a
  way to know which pages their neighbours are showing. The heartbeat's page
  byte is reserved and sent as zero, which is exactly where that would go.

## 5. Milestones

Each has an explicit exit criterion. **M1 and M2 are independent and can run
in either order.**

| # | Milestone | Exit criterion |
|---|---|---|
| **M0** | **One** RP2350-Touch-AMOLED-1.75 ordered 2026-09-04 — deliberately one, not three, so node 1 proves the design before the other two are committed. Transceivers already on hand (spare TJA1051T/3 breakouts). Still to order: carrier parts | Board on the bench; module outline measured against the 180 x 50 mm aperture (§4.8); the 8-pin header found to carry CAN, power, ground and the ID pin together (§4.1); no GNSS populated (confirmed); vendor `05_LVGL` draw-buffer allocation read in source |
| **M1** | SAMC21 scaling: `ecu_scale.c`, host tests, new frame layout, DBC + `can_monitor.py` updated | `can_monitor.py` shows correct engineering units for every signal against the running bench rig |
| **M1a** | Implement the §3.6 temperature curve as a 256-entry build-time LUT; resolve the §3.6b ignition ambiguity | LUT reproduces the formula to <0.15 degC across X = 0..248; host test asserts the 84 measured points in `temp_sensor_calibration.xlsx` to within 1.3 degC; curve copied into `adc_system.md`; ignition scale settled against the disassembly. **No bench characterisation needed — the calibration data already exists.** |
| **M2** | Bring-up on the stock dev board from the vendor `01_GUI`/`05_LVGL` examples: pico-sdk + CMake, CO5300 QSPI panel under LVGL 8.1, converted to **partial buffers** (§4.1a) | A test pattern rendering full-screen; free SRAM measured and recorded; PIO block assignment fixed with the panel and can2040 on different blocks |
| **M3** | can2040 receive + DBC decode | Live RPM from the bench rig printed over USB serial |
| **M4** | **Timing validation — the go/no-go** | Zero CAN bit errors over a sustained run with LVGL rendering flat out on core 1, **and** render throughput unchanged from the idle-bus baseline. Work the §4.2a levers in order before concluding failure. If it still fails: switch to MCP2518FD **before** layout |
| **M5** | Widget library, page list, swipe paging with persistence, staleness handling | Every page renders from the table on live data; swipe changes page and the choice survives a power cycle; a fault forces the warning face regardless of selection; unplugging CAN visibly degrades rather than freezing |
| **M6** | Node 1 complete on dev hardware | A finished gauge running on the bench for an extended session |
| **M7** | Choose the in-car panel size (§4.8); design and build the **carrier** (12 V protection, buck to 5 V, TJA1051, ID divider, loom connector) and the **180 x 50 mm fascia**; build nodes 2 and 3 | Three identical nodes, one flash image, identity by resistor value only; fascia holds all three with the glass flush or proud, not recessed |
| **M8** | In-car install | Loom (12 V, GND, CANH, CANL daisy chain, one connector type), termination checked, all three nodes live |

---

## 6. Open questions

1. **Can the 1.75" module mount with its glass presenting through a
   180 x 50 mm fascia?** Waveshare publish no outline drawing, and the active
   circle leaves only 2.8 mm of margin in a single-DIN aperture (§4.8).
   Measure the real board on arrival. If the answer is no, the fallback is the
   1.43" at the same 466 x 466 — no firmware changes.
2. ~~**Does the SH1.0 connector carry a usable ground alongside the UART
   pins?**~~ **Moot, 2026-09-19.** The 8-pin 0.1" header carries RXD, TXD,
   3V3, GND, VBUS and the ID pin together, so the SH1.0 plug is not needed at
   all - see §4.1.
3. ~~Does the vendor's LVGL example under-allocate its draw buffer?~~
   **Answered 2026-09-12: yes, by a factor of two, a 212 KB heap overflow.**
   See section 4.1a. Its panel and touch drivers are still the ones to port;
   its LVGL setup is not.
4. **Which identifiers does the 14Point7 use, under which protocol
   profile?** Bit rate is settled (configurable, set it to 500 kbit/s), so
   only the identifiers remain. Log the bench bus under each candidate profile
   and pick one clear of the Toyotune ranges (§4.9) — that is likely cheaper
   than moving `TOYOTUNE_CAN_ID_TELEMETRY_BASE`.
5. **Independent or coordinated paging?** Swiping is per-node, so two nodes
   can end up on the same page. Options: (a) leave it — a duplicate was the
   driver's own choice; (b) a node seeing a neighbour's page in its heartbeat
   (§4.7) skips to the next unclaimed one; (c) one swipe rotates all three
   together. **(a) is the right default** — simplest, most predictable, and
   the heartbeat byte keeps (b) available later without a protocol change.
   Decide before M5.
6. **Which CPU does each gauge listen to?** Both boards publish the same
   signal names on `0x400` and `0x420`. Note `Rpm` reaches CPU1's frame from
   *CPU2's* DMA block, so `0x400` and `0x420` will carry near-identical RPM —
   pick one source per signal deliberately.
7. **Will the second Toyotune board (CPU2) exist by then?** `config.h` still
   `#error`s on `TOYOTUNE_CPU2` pending its own ROM image in `image.c`. The
   dash must work with only CPU1 present.
8. **TPS endpoints** — fixed calibration constants, or learned closed/WOT?
9. **AMOLED burn-in policy.** Brightness is settled — AMOLED answers the
   readability problem an IPS panel would have had. What is not settled is how
   to protect a *static* gauge face: dim aggressively, shift elements by a few
   pixels periodically, or both. Dash power being ignition-switched limits the
   exposure, but decide the policy before M5 rather than after a face has
   burned in. Night dimming (§4.10) cuts exposure for every hour driven after
   dark, which bears on how aggressive the daytime policy needs to be.
   Meanwhile bench builds boot at 20% (`DASH_BRIGHTNESS`) so a development
   board left running does not burn the dial in.

---

## 7. Risks

| Risk | Impact | Mitigation |
|---|---|---|
| can2040 bit errors under render load | Rework to MCP2518FD after layout | **M4 is a hard gate before PCB** |
| Ignition timing scaled wrong (§3.6b) | A timing display that is plausibly but silently wrong | Resolve the two conflicting pairs against the disassembly before implementing; prefer the 16-bit `IgnAdvanceHi`/`Lo` source |
| LUT quantisation or clamp errors on temperature | Gauge wrong at the extremes | Host unit tests against the three validated points in §3.6; clamp to `[0x03,0xF8]` |
| Endianness slip on computed values | Wildly wrong 16-bit signals | Explicit big-endian write helper; assert in host tests |
| Scope creep into per-node code | "Replicate twice" stops being cheap | Identity on a resistor, faces as data, one flash image — enforce it |
| 466x466 partial rendering too slow or too tight | Sluggish or unstable gauge | Measured at M2 before three boards are committed; reserved PSRAM pad is the fallback (§4.1a) |
| Touch unreachable once the panels are in pods | The whole paging feature dies after the bezel is built | Settle bezel/cover design against touch reach at M0, alongside the outline dimensions |
| Spurious page changes from water or a wet fingertip | Gauge changes while driving | Distance + velocity thresholds on the swipe; taps do nothing; a fault always forces the warning face |
| 14Point7 identifier clashes with Toyotune telemetry | Two nodes transmitting the same ID — arbitration errors and garbage decode | Log the bench bus before wiring it in; first try a different protocol profile on the wideband, and only then move `TOYOTUNE_CAN_ID_TELEMETRY_BASE` (§4.9) |
| Frame layout drifts from the DBC | Silent mis-decode | `INFO` frame carries a protocol version; dash reports mismatch on screen |

---

## 8. References

- can2040 API and requirements — <https://github.com/KevinOConnor/can2040/blob/master/docs/API.md>
- RP2350 erratum E9 discussion — <https://forums.raspberrypi.com/viewtopic.php?t=375631>
- can2040 code overview (PIO/IRQ structure, no DMA) — <https://github.com/KevinOConnor/can2040/blob/master/docs/Code_Overview.md>
- Bus priority on RP2350 vs RP2040 — <https://github.com/raspberrypi/pico-sdk/issues/2123>
- RP2350-Touch-AMOLED-1.75 product page — <https://www.waveshare.com/rp2350-touch-amoled-1.75.htm>
- RP2350-Touch-AMOLED-1.75 documentation — <https://docs.waveshare.com/RP2350-Touch-AMOLED-1.75>
- C/C++ environment, examples and LVGL notes — <https://docs.waveshare.com/RP2350-Touch-AMOLED-1.75/Development-Environment-Setup-VSCode>
- The 1.43" sibling, same 466x466 on a smaller panel — <https://thepihut.com/products/rp2350-1-43-amoled-round-display-dev-board-466x466>
