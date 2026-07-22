# ADC System — 3S-GTE ECU CPU1 (D151803-9651)

## Overview

CPU1 uses the serial port (SIN0/SOUT0) as an ADC multiplexer interface. An external ADC is connected to the serial lines. CPU1 sends a channel selection command each interrupt cycle and receives back the ADC result from the *previously* requested channel — so results always lag one cycle behind the command.

The serial port operates in 9-bit frame mode. The parity/channel bit (SSD.1) distinguishes ADC channel reads (mark, SSD.1=1) from diagnostic exchanges (space, SSD.1=0).

**Command encoding:** raw command byte `B` is encoded as `(B << 1) | 1) & 0x1F` before transmission — bit 0 is always set, bits 4..1 carry the channel number.

---

## ADC Channel Map

| Command | Handler | Signal | Description |
|---|---|---|---|
| 0x00 | `adc_handler_pim` | PIM | MAP / turbo boost pressure |
| 0x01 | `adc_handler_tps` | TPS | Throttle position (primary) |
| 0x02 | `adc_handler_o2_heater` | O2H | O2 sensor heater current |
| 0x03 | `adc_handler_battery` | VB | Battery voltage |
| 0x04 | `adc_handler_ect` | ECT | Engine coolant temperature |
| 0x05 | `adc_handler_tha` | THA | Intake air temperature |
| 0x06 | `adc_handler_iscv_pos` | ISCV | ISCV position feedback |
| 0x07 | `adc_handler_iscv_fb` | ISCV | ISCV feedback channel 2 |
| 0x08 | `adc_handler_trac_tps` | TRAC-TPS | Secondary TPS (TRAC system) |
| 0x09 | `adc_handler_iscv_3` | ISCV | ISCV feedback channel 3 |
| 0x0A | `adc_handler_tham` | THAM | Manifold air temperature |
| 0x0B | `adc_handler_complete` | — | End-of-scan marker |
| 0x0C | `adc_handler_o2_sensor` | O2 | O2 sensor voltage |
| 0x0D | `adc_handler_iscv_4` | ISCV | ISCV feedback channel 4 |

---

## Scan Phases

### Phase 1 — Initial Sequential Scan

On startup (`var_flags_42.1 = 0`), commands 0x00..0x0D are issued in sequence. This ensures all 14 channels are read at least once before normal operation begins. When command 0x0D is complete, `var_flags_42.1` is set and Phase 2 begins.

### Phase 2 — Scheduled Scan

`var_adc_idx` cycles 0..127, incrementing each interrupt. The lower 3 bits (slot 0..7) index an 8-entry schedule table. Two tables exist — selection depends on `unk_40.0` and `unk_40.1` flags (TRAC active / A/T mode):

#### `table_adc_ch_normal` — Standard Schedule

| Slot | Entry | Action |
|---|---|---|
| 0 | 0x82 | SKIP |
| 1 | 0x02 | O2 heater |
| 2 | 0x81 | LOW-PRI lookup |
| 3 | 0x80 | DIAG (send 0xDA) |
| 4 | 0x82 | SKIP |
| 5 | 0x08 | TRAC TPS |
| 6 | 0x01 | TPS (primary) |
| 7 | 0x80 | DIAG (send 0xDA) |

#### `table_adc_ch_trac` — TRAC/A/T Variant

Identical to normal except slot 1 = 0x03 (Battery voltage) instead of O2 heater.

#### Special Slot Values

| Value | Meaning |
|---|---|
| 0x80 | Send diagnostic byte 0xDA; read and discard response |
| 0x81 | Use `table_adc_ch_low_pri` — see below |
| 0x82+ | Skip this slot; no command sent |

### Low-Priority Channel Schedule

When slot 2 is reached (entry 0x81), the upper bits of `var_adc_idx` select a group from `table_adc_ch_low_pri`. Group index = `(var_adc_idx >> 3) & 0x0F`, cycling through 16 possible groups. The table holds 4 groups × 4 entries (0x10 bytes total):

| Group | Channels |
|---|---|
| 0 | ECT, THAM, Battery, ISCV-pos |
| 1 | ECT, THA, Battery, ISCV-fb |
| 2 | ECT, THAM, Battery, ISCV-3 |
| 3 | ECT, ISCV-4, Battery, O2-sensor |

Each low-priority channel is sampled once every 8 interrupt cycles (one slot 2 per 8-slot cycle). A full rotation through all 4 groups takes 32 interrupt calls.

### Effective Sampling Rates

| Channel | Approximate rate | Note |
|---|---|---|
| TPS | Every 8 ADC cycles | Slot 6, high priority |
| TRAC TPS | Every 8 ADC cycles | Slot 5, high priority |
| O2 heater / Battery | Every 8 ADC cycles | Slot 1 (alternates by table) |
| ECT | Every 8 ADC cycles | Appears in every low-pri group |
| THA / THAM | Every 32 ADC cycles | One group each |
| Battery | Every 8 ADC cycles | Present in all low-pri groups |
| O2 sensor | Every 32 ADC cycles | Group 3 only |
| ISCV channels | Every 32 ADC cycles | One group each |

---

## ADC Handler Details

### `adc_handler_pim` — MAP / Boost Pressure

**Input:** X = raw 16-bit ADC from MAP sensor

**Scaling:**
```
mPIM = raw_adc - 10560          ; subtract 1-bar atmospheric baseline (0x2940 = ~0.8V)
var_pim2 = mPIM * 1.285156      ; scale by 329/256 (converts sensor units to pressure)
```
`var_pim2 = 0` at atmospheric pressure. Increases linearly with boost.

**Sensor limits:** `pim_adc_limits = [0x1A, 0xE6]` → valid range 0.51V..4.51V

**On out-of-range:**
- Default value: `0x6666`
- First occurrence: sets `var_flags_18C.0`, resets error counter
- After 15 consecutive errors: sets `var_error_flags1.5`, `var_diag_errors_5.3`

**PIM trim (PRAM):** `var_nv_trim_unk_98` is a calibration trim stored in battery-backed PRAM. It is updated during stable idle conditions and used to adjust the MAP reading.

**Boost pressure index:** `unk_144 = var_pim2 / 2 / 97` — used as a lookup index in `check_boost_limit`.

**Outputs:** `var_pim2` (local), `dmatx_pim2` (→ CPU2), calls `check_boost_limit`

---

### `adc_handler_o2_heater` — O2 Sensor Heater Current + Lambda Integrator

**Input:** A = high byte of 16-bit ADC result

**O2 heater scaling:**
```
var_adc_o2_heater = A * 4, saturated to 0xFF
  A = 0..63  → result = 0..252
  A > 63     → result = 0xFF
```

**Lambda integrator (`var_adc_lambda`):**

The lambda integrator is a signed accumulator that drives closed-loop fuel correction. It is updated from `table_adc_lambda_C249`:

```
table_adc_lambda_C249:
  Rich steps (throttle closed):   +0x09, +0x09, +0x06, +0x10, +0x0B
  Lean steps (throttle open):     -0x02, -0x02, -0x02, -0x02, -0x02
```

- **Throttle closed** (`var_flags_46.2` set): uses rich-direction step. The integrator increments toward a rich target.
- **Throttle open**: O2 sensor voltage (bits 1..0 of `var_adc_o2_sensor` shifted) selects which lean step entry to use. The integrator decrements toward lean.

The crossover voltage is 0x170A (raw 16-bit): above this = lean signal; below = rich signal.

**Outputs:** `var_adc_o2_heater`, `var_adc_lambda` (local + `dmatx_adc_lambda` → CPU2)

---

### `adc_handler_tps` — Throttle Position Sensor (Primary)

**Input:** B = raw 8-bit TPS ADC value

**Valid range:** 0x0D..0xFB (raw byte, ≈0.25V..4.92V)

**Throttle state detection using IDL signal (`var_io_input1.1`):**

| Condition | State |
|---|---|
| IDL high AND B < 0x0D | Out-of-range low (wire off / fully closed) |
| IDL high AND 0x0D ≤ B < 0x31 | Fully closed — set `var_flags_18C.5` (closed-loop enable) |
| IDL high AND 0x31 ≤ B < 0x4D | Closing — clear `var_error_flags2.1` |
| IDL high AND B ≥ 0x4D | IDL inconsistent with TPS — out-of-range high path |
| IDL low | Throttle open — clear `var_flags_18C.5` |
| B > 0xFB | Out-of-range high |

**On out-of-range:**
- `var_tps` and `dmatx_tps` set to 0x0000
- After 15 consecutive reads: sets `var_diag_errors_5.3`, `var_error_flags2.1`

**NV TPS trim:** `var_nv_tps` (PRAM) is a learned closed-throttle position used as a reference for TPS scaling.

**Outputs:** `var_tps_raw` (raw byte), `var_tps` (16-bit, 0 if error), `dmatx_tps` → CPU2

---

### `adc_handler_trac_tps` — Secondary TPS (TRAC System)

**Input:** B = raw 8-bit ADC from secondary throttle body

**Activation conditions** (all must be true):
- `var_ignition_flags.6` set (engine running)
- `var_4ms_cnt_B9 > 0x18` (post-start settle period elapsed)
- B ≥ 0xF0 (secondary throttle ≥ 94% open)

If not active: `unk_1DC.2` cleared, `var_trac_tps_scaled` not updated.

**If active:**
- Same bounds check as primary TPS (0x0D..0xFB)
- Closed-throttle detection uses PORTA.2 (TE1 input)
- NV trim `unk_302` (PRAM, limits `nv_302_limits = [0x14, 0xC3]`) used for calibration
- Error after 15 reads: sets `var_diag_errors_5.3`, `var_error_flags2.3`

**Outputs:** `var_trac_tps_raw`, `var_trac_tps_scaled`

---

### `adc_handler_tha` — Intake Air Temperature (THA)

**Input:** B = raw 8-bit ADC from NTC thermistor

**Sensor limits:** `tha_adc_limits = [0x07, 0xFC]` → valid range 0.14V..4.94V

**Encoding:** NTC thermistor produces high voltage when cold. Raw value is XOR 0xFF so:
- `var_tha` high = hot
- `var_tha` low = cold

**On out-of-range:**
- Default: B = 0x79, then XOR 0xFF = 0x86 (≈ 20°C)
- First occurrence: sets `var_flags_18C.2`, resets error counter
- After 15 consecutive errors: sets `var_error_flags1.4`, `var_diag_errors_5.3`

**Outputs:** `var_tha = B XOR 0xFF`, `dmatx_tha` → CPU2

---

### `adc_handler_tham` — Manifold Air Temperature (THAM)

**Input:** B = raw 8-bit ADC from NTC thermistor

**Sensor limits:** `tham_adc_limits = [0x07, 0xFC]` (same as THA)

Identical structure to THA. Separate error flag bits:
- First error: `var_flags_18D.0`
- Error flags: `var_error_flags2.4`, `var_diag_errors_5.3`
- Default: 0x79 XOR 0xFF = 0x86 (≈ 20°C)

**Outputs:** `var_tham = B XOR 0xFF`, `dmatx_tham` → CPU2

---

### `adc_handler_ect` — Engine Coolant Temperature (ECT)

**Input:** B = raw 8-bit ADC from NTC thermistor. Result promoted to 16-bit.

**Sensor limits:** `ect_adc_limits = [0x07, 0xFC]`

**Encoding:** Both bytes XOR 0xFF; lower byte masked to 0xC0 (top 2 bits only):
```
A = A XOR 0xFF    ; invert high byte
B = B XOR 0xFF    ; invert low byte
B = B AND 0xC0    ; keep only top 2 bits of low byte (10-bit effective resolution)
var_ect = A:B
```

**On out-of-range:**
- Default: X = 0x1B00 → after XOR+mask: A=0xE4, B=0x00 → `var_ect = 0xE400` (≈ 82°C)
- First error: `var_flags_18C.1`
- After 15 errors: `var_error_flags1.3`, `var_diag_errors_5.3`

**Outputs:** `var_ect` (16-bit, XOR-inverted + masked), `dmatx_ect` → CPU2

---

### `adc_handler_battery` — Battery Voltage

**Input:** B = raw 8-bit ADC (voltage divider, measuring 0–~15V)

**Voltage threshold `0x53` (≈ 6.5V):**
If battery ≥ 6.5V AND PRAM valid AND init flag (`unk_44.6`) set → release knock MCU reset (assert DOUT.2 high). This prevents the knock MCU from starting until the battery is stable post-crank.

**Injector battery compensation:**

`table_inj_battery_adjust` provides a correction for injector open time at low battery voltage (injectors open more slowly when voltage is low, requiring longer pulse width):

| Battery voltage | ADC raw |
|---|---|
| 6.5V | 0x53 |
| 8.95V | 0x4E |
| 11.44V | 0x2A |
| 13.92V | 0x1E |
| 16.40V | 0x11 |

```
var_inj_battery_adjust = table_rB_fixed_32_interpolate(B) / 64
```

**Outputs:** `var_adc_battery` (raw), `dmatx_battery` → CPU2, `var_inj_battery_adjust`

---

### `adc_handler_o2_sensor` — O2 Sensor Voltage

**Input:** B = raw 8-bit ADC from lambda sensor

Stores raw reading only. Used by `adc_handler_o2_heater` in the subsequent lambda integrator update to select the appropriate lean step from `table_adc_lambda_C249`.

**Output:** `var_adc_o2_sensor`

---

### `adc_handler_trac_tps` (channel 0x08) vs `adc_handler_tps` (channel 0x01)

Both handlers share the same bounds (0x0D..0xFB) and IDL-based closed-throttle logic, but differ in:
- Input signal: primary vs secondary throttle body
- TRAC TPS activation guard (engine running, settle time, ≥94% open condition)
- NV trim variable: `var_nv_tps` vs `unk_302`
- Error flag bits: `var_error_flags2.1` vs `var_error_flags2.3`

---

### ISCV Feedback Channels

Channels 0x06, 0x07, 0x09, 0x0D are low-priority channels that store raw 8-bit ADC values with no processing. Based on their grouping in the low-priority schedule and their relationship to the idle speed control system, these are tentatively identified as idle speed control valve (ISCV) feedback signals.

| Channel | Variable | Probable function |
|---|---|---|
| 0x06 | `var_adc_iscv_pos` | ISCV valve position feedback |
| 0x07 | `var_adc_iscv_fb` | ISCV secondary feedback |
| 0x09 | `var_adc_iscv_3` | ISCV channel 3 |
| 0x0D | `var_adc_iscv_4` | ISCV channel 4 |

These are not transmitted to CPU2 via DMA and appear to be used locally in idle speed control calculations only.

---

## Variable Reference

### RAM Variables

| Variable | Description |
|---|---|
| `var_adc_cmd` | ADC command currently being sent (= channel of *next* result) |
| `var_adc_idx` | Phase 2 schedule index (0..127) |
| `var_pim2` | Scaled MAP/boost pressure (0 = 1 bar) |
| `var_tps_raw` | Raw TPS ADC byte |
| `var_tps` | TPS value (16-bit, 0x0000 if sensor error) |
| `var_trac_tps_raw` | Raw TRAC TPS ADC byte |
| `var_trac_tps_scaled` | Processed TRAC TPS value |
| `var_tha` | Intake air temperature (XOR-inverted, high = hot) |
| `var_tham` | Manifold air temperature (XOR-inverted, high = hot) |
| `var_ect` | Coolant temperature (XOR-inverted 16-bit, high = hot) |
| `var_adc_battery` | Raw battery ADC byte |
| `var_inj_battery_adjust` | Injector pulse width battery compensation |
| `var_adc_o2_sensor` | Raw O2 sensor voltage |
| `var_adc_o2_heater` | Scaled O2 heater current (0..0xFF) |
| `var_adc_lambda` | Lambda integrator (signed, drives fuel trim) |
| `var_adc_iscv_pos` | ISCV position feedback |
| `var_adc_iscv_fb` | ISCV feedback channel 2 |
| `var_adc_iscv_3` | ISCV feedback channel 3 |
| `var_adc_iscv_4` | ISCV feedback channel 4 |
| `var_cnt_sensor_error` | Consecutive out-of-range read counter (threshold = 15) |
| `var_flags_18C` | Sensor error status flags (PIM.0, ECT.1, THA.2) |
| `var_flags_18D` | Sensor error status flags (THAM.0) |

### PRAM Variables (battery-backed, persist over ignition off)

| Variable | Description |
|---|---|
| `var_nv_trim_unk_98` | PIM calibration trim value |
| `var_nv_tps` | Learned TPS closed-throttle position |
| `unk_302` | Learned TRAC TPS trim value |

### DMA Transmit Variables (→ CPU2 every 4ms)

| Variable | Description |
|---|---|
| `dmatx_pim2` | Boost pressure |
| `dmatx_tps` | Throttle position |
| `dmatx_tham` | Manifold air temperature |
| `dmatx_tha` | Intake air temperature |
| `dmatx_ect` | Coolant temperature |
| `dmatx_battery` | Battery voltage |
| `dmatx_adc_lambda` | Lambda integrator value |

---

## Error Handling Pattern

All sensors with out-of-range detection follow the same pattern:

```
1. Clamp raw ADC to valid range via clamp_rB / limits table
2. If C set (clamped = out of range):
     a. Check if error already flagged in var_flags_18x
     b. If first occurrence: set flag, reset var_cnt_sensor_error
     c. If already flagged: increment var_cnt_sensor_error
     d. If var_cnt_sensor_error >= 15: set error flags (diag + operational)
     e. Use default value for this cycle
3. If C clear (in range):
     a. Clear error flag in var_flags_18x
     b. Clear operational error flag (if not blocked by unk_40.2)
     c. Use actual ADC value
```

The `unk_40.2` flag gates error flag clearing — when set, existing errors are preserved even when readings return to range.

---

## Data Flow Diagram

```
External ADC (SIN0/SOUT0 serial link)
  |
  | 9-bit serial frame (command N-1 sent, result N-1 received)
  v
int_vector_1_serial_rx  [triggered by SSD.7 = RX buffer ready]
  |
  |-- Phase 1 (startup): sequential commands 0x00..0x0D
  |-- Phase 2 (normal):  scheduled via table_adc_ch_normal / table_adc_ch_trac
  |
  v
table_adc_handler[prev_command]  --> dispatch to channel handler
  |
  +-- adc_handler_pim      --> var_pim2,          dmatx_pim2
  +-- adc_handler_tps      --> var_tps,            dmatx_tps
  +-- adc_handler_o2_heater--> var_adc_o2_heater, var_adc_lambda, dmatx_adc_lambda
  +-- adc_handler_battery  --> var_adc_battery,   dmatx_battery, var_inj_battery_adjust
  +-- adc_handler_ect      --> var_ect,            dmatx_ect
  +-- adc_handler_tha      --> var_tha,            dmatx_tha
  +-- adc_handler_tham     --> var_tham,           dmatx_tham
  +-- adc_handler_o2_sensor--> var_adc_o2_sensor
  +-- adc_handler_trac_tps --> var_trac_tps_scaled
  +-- adc_handler_iscv_*   --> var_adc_iscv_*
  |
  v
adc_complete --> reti

DMA frame (every 4ms)
  dmatx_* variables --> CPU2 --> fuel/ignition calculations
```

---

*Derived from IDA disassembly of D151803-9651 (CPU1), Toyota 3S-GTE ECU.*
