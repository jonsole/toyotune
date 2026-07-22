# Ignition Control System — 3S-GTE ECU CPU1 (D151803-9651)

## Overview

The ignition system uses the D8X timer's CPR0 compare register to schedule coil events precisely in real time. Ignition advance is computed on every NE pulse inside `iv6_ne_process`, expressed as a time offset from the current ASR2 crank timestamp, and then programmed into CPR0. The `int_vector_9_ignition` interrupt fires when CPR0 matches TIMER, toggling the coil.

**Coil driver (DOUT.0):**
- `DOUT.0 = 0` → coil charging (coil on, current building)
- `DOUT.0 = 1` → coil firing (coil off, spark discharged)

The coil charges from one CPR0 event and fires at the next. The dwell angle is therefore the time between the coil-on CPR0 and the coil-off CPR0.

**IGF (Ignition Feedback):** The spark confirmation signal from the igniter arrives on ASR0 and is monitored via IRQLL.4. Missing IGF events are counted and can trigger fuel cut.

---

## Timing Units

All ignition timing values are stored in a common unit derived from the 19-bit hardware timer (TIMERC):

| Quantity | Resolution | Formula |
|---|---|---|
| TIMER register | 4 µs per count | TIMERC / 8 |
| CPR0 register | 4 µs per count | Same as TIMER |
| `var_ign_advance_raw` | ~0.5° per count | degrees × (ne_sum3 / 45) |
| `var_ign_timing_div_2` | ~0.5° per count | intermediate timing value |

**Degree–to–timer-unit conversion:**
```
ne_sum3 = time for 45 crank degrees (3 NE pulses × 15°/pulse) in 4 µs units
1 crank degree = ne_sum3 / 45 timer units

timing_offset_units = timing_degrees × ne_sum3 / 45
```

This conversion is performed by `ignition_timing_to_cpr` (formerly `sub_F263`).

**Timing offset reference points:**
| Value | Meaning |
|---|---|
| 0x2B (43) | Zero degrees BTDC reference (stored in `var_ign_timing_div_2`) |
| 0x4B (75) | Zero degrees OBD1 reference |
| 0x1CD (461) | Maximum advance clamp = 90° BTDC |
| 0x55 (85) | Minimum advance (after knock retard subtraction) |
| 0x07 (7) | Small forward offset added to CPR advance to account for processing latency |

---

## Hardware Signals

| Signal | Pin | Direction | Description |
|---|---|---|---|
| IGT | DOUT.0 | Output | Ignition trigger to igniter (coil on/off) |
| IGF | ASR0 | Input | Ignition feedback confirmation from igniter |
| CPR0 | — | Internal | Timer compare register for coil event scheduling |

---

## `iv6_ne_process` — Ignition Scheduling (called every NE pulse)

`iv6_ne_process` runs as a software interrupt (IV6) triggered by `int_vector_e_ne`. It performs NE period measurement, RPM calculation, knock processing, and ignition scheduling on every one of the 24 NE pulses per revolution.

The ignition scheduling section operates on NE positions 1, 3, and 4:

### Position 1 — Dwell Angle Adjustment

At NE position 1, if RPM ≥ 3000 (`var_rpm_div_25 ≥ 0x3C`) and `var_ignition_flags.2` is clear:

```
dwell_end_time = prev_asr2_time
              + (var_ign_timing_div_2 - var_ign_advance_raw) / 2 × ne_2
              - 0x35
              - var_ign_dwell_offset
```

This pre-positions CPR0 to fire the dwell-end event early enough for the next cylinder, accounting for current engine speed.

### Position 3 — Compute Ignition Timing and Schedule Coil Fire

This is the main timing computation. It selects the appropriate advance value and computes the CPR0 offset for the spark event.

#### Step 1: Select base timing source

| Condition | Timing source |
|---|---|
| `var_ignition_flags.2` set (limp mode) | Use `var_ign_cold_advance` (cold/emergency value) |
| Normal | Use `var_ign_timing_div_2` (computed advance from CPU2) |

#### Step 2: Apply override conditions

| Condition | Override |
|---|---|
| `var_flags_46.7` set (knock error) | Force -5° BTDC (raw 86) |
| `unk_40.0` set | Use normal computation |
| `var_flags_4D.3` set (default PIM) | Force -5° BTDC |
| `var_flags_46.2` set AND diagnostic mode active | Force -10° BTDC (raw 128) |

#### Step 3: Apply minimum timing clamp

```
if timing < var_ign_timing_min:
    timing = var_ign_timing_min
```

#### Step 4: Apply knock retard

```
retard = (var_ign_knock_retard_base << 1) - dmatx_knock_retard
if retard < 0: retard = 0
timing -= retard
if timing < 0: timing = 0
```

#### Step 5: Apply upper advance clamp

```
if timing > var_ign_advance_max: timing = var_ign_advance_max
if timing > var_ign_advance_raw: timing = var_ign_advance_raw
```

#### Step 6: Add RPM-based advance trim from CPU2

Selects between two DMA-received advance values based on `va_ne_count_2`:
```
if va_ne_count_2 >= 0x30:
    timing += dmarx_ign_advance_hi
else:
    timing += dmarx_ign_advance_lo
```

These are the RPM and load-based advance angles computed by CPU2 from the fuel/ignition maps.

#### Step 7: Apply maximum clamp

```
if timing > 0x1CD: timing = 0x1CD    ; 90° BTDC maximum
```

#### Step 8: Convert OBD format and store

```
dmatx_ign_obd = (timing >> 1) + 0x35
```

OBD1 ignition timing format: value 0x35 (53) = 0° BTDC, each count = 0.5°.

#### Step 9: Subtract processing latency and clamp

```
timing -= 0x07
if timing < 0: timing = 0
```

#### Step 10: Convert degrees to timer units

Using `ignition_timing_to_cpr`:
```
cpr_offset = timing_degrees × ne_sum3 / 45
```

Implemented as:
```
D = (A / 6) × ne_sum3   (main term)
D += (B / 6) × var_ign_ne_frac   (fractional correction)
```

#### Step 11: Compute CPR0 fire time

```
cpr_fire = var_asr2_time + ne_sum3 - 0x35 + cpr_offset
```

Where `var_asr2_time` is the ASR2 timestamp of the current NE pulse.

This is passed to `ignition_schedule_off` (formerly `sub_F229`) which arms CPR0 for the spark event and sets DOUT.0 = 1 (DOM-latched at CPR0 match).

---

## `int_vector_9_ignition` — CPR0 Match Interrupt

Fires when TIMER matches CPR0. On each event it checks `LDOUT.0` to determine whether the coil just turned on or off:

### Coil just turned off (LDOUT.0 = 1 → spark just fired)

1. Clear `var_ignition_flags.0`
2. If `var_ignition_flags.5` is set (pending on-time was set during coil fire): schedule next coil-on event via `ignition_set_on_time`, then process IGF

### Fixed dwell fallback (coil still on, no pending on-time)

If `var_ignition_flags.5` is clear (normal dwell not computed yet):
```
CPR0 += 16250    ; 16250 × 4µs = 65ms fixed dwell
DOUT.0 = 0       ; Schedule coil fire (via DOM at CPR0 match)
```

This 65ms fallback dwell fires the coil if the normal scheduling code hasn't set a dwell time. It prevents misfires at very low RPM or during startup.

### Coil just turned on (LDOUT.0 = 0 → coil charging)

1. Save CPR0 value to `var_ign_coil_on_time` (used later for dwell feedback)
2. If not starter running: call `ignition_update_off_time` to refine the dwell endpoint based on the actual coil-on time
3. Compute adjusted dwell end time accounting for coil inductance and battery voltage

### IGF monitoring (inside the coil-off path)

After each spark event, the IGF signal (ASR0 input) is checked:

```
if IRQLL.4 set (IGF received):
    clear IGF fuel-cut flag
    clear IGF error flag
    reset miss counter to 1
else:
    increment var_igf_miss_count (clamped at 0xFF)
    if var_igf_miss_count >= 5:
        set var_limiter_flags.4 → FUEL CUT (missing IGF)
    if RPM ≥ 3000 AND battery ≥ 8.9V AND not starter:
        increment igf_error_count
        if igf_error_count >= 9 AND var_4ms_cnt_igf_timer >= 0x3F:
            set var_error_flags1.2 (IGF missing error)
            set var_flags_4D.4 (too many missing IGF)
```

**IGF RPM threshold:** Below 3000 RPM the IGF miss count is allowed to be 1 before logging (misfires at cranking speed are tolerated). Above 3000 RPM any miss is counted.

**IGF battery threshold:** 0x73 ≈ 8.9V. Below this voltage the IGF monitoring is suppressed (weak battery may cause igniter issues).

---

## `ignition_schedule_off` — Schedule Coil Fire Event

Arms CPR0 for the spark event (DOUT.0 = 1 via DOM):

```
var_ign_next_cpr = X    ; save requested fire time
di                       ; disable interrupts
if DOUT.0 == 0:          ; coil already off (spark already fired?)
    CPR0 = TIMER + 4     ; fire immediately (add 16µs safety margin)
else:
    DOUT.0 = 1 via DOM   ; set coil-off to occur at CPR0 match
    CPR0 = var_ign_next_cpr - 4  ; subtract 16µs processing offset
    if CPR0 <= TIMER:    ; already past?
        CPR0 = TIMER + 4 ; fire immediately
ei
```

## `ignition_schedule_on` — Schedule Coil Charge Event

Arms CPR0 for the coil-on event (DOUT.0 = 0 via DOM):

```
var_ign_next_cpr = D
di
if var_ignition_flags.3 set:
    call ignition_set_on_time immediately
else:
    set var_ignition_flags.5 (pending on-time)
ei
```

## `ignition_timing_to_cpr` — Degrees to Timer Units

Converts ignition advance in degrees to CPR timer offset units, accounting for current engine speed:

```
; Entry: A = timing_degrees_hi, B = timing_degrees_lo (scaled)
; ne_sum3 = 45° period in 4µs units

result = A × ne_sum3 / 6
result += B × var_ign_ne_frac / 256    ; fractional correction
```

The `/6` comes from the relationship: `ne_sum3 / 45 = ne_sum3 / (6 × 7.5)`. Since the timing is stored in units where 6 counts ≈ 1 degree, this produces the correct timer-unit offset.

---

## `ignition_update_off_time` — Dwell Endpoint Correction

Adjusts the coil-fire CPR0 time based on actual coil-on time:

```
D -= var_ign_dwell_offset
if RPM < threshold (ne_sum3 < 0x1D):    ; below ~2000 RPM
    call ignition_schedule_off directly (D is the fire time)
else:
    elapsed = D - var_ign_coil_on_time
    if elapsed >= 0 and elapsed/2 >= var_ign_dwell_min:
        call ignition_schedule_off
    else:
        ; dwell too short: extend to minimum
        D = var_ign_coil_on_time + (var_ign_dwell_min << 1)
        call ignition_schedule_off
```

---

## `check_IGF_error` — IGT/IGF Timer Check

Called from the 4ms main loop. Monitors how long the ignition trigger has been active. If `var_flags_4D.4` is clear, resets `var_igt_timer`. Otherwise checks if the timer has exceeded 0x2E (about 184ms) and sets an error flag.

---

## Data Flow Diagram

```
CPU2 (ignition map calculations)
  │  dmarx_ign_advance_hi / dmarx_ign_advance_lo
  │  (RPM + load based advance, received via 4ms DMA)
  ▼
iv6_ne_process  [runs every NE pulse = 24× per revolution]
  │
  ├─ Select base advance: var_ign_timing_div_2 (normal) or var_ign_cold_advance
  ├─ Apply error overrides: -5° or -10° fixed timing if fault conditions
  ├─ Clamp to var_ign_timing_min
  ├─ Subtract knock retard: var_ign_knock_retard_base - dmatx_knock_retard
  ├─ Clamp to var_ign_advance_max, var_ign_advance_raw
  ├─ Add CPU2 RPM advance: dmarx_ign_advance_hi or dmarx_ign_advance_lo
  ├─ Clamp to 90° BTDC max
  ├─ Encode OBD format → dmatx_ign_obd
  ├─ Convert to timer units: ignition_timing_to_cpr
  ├─ Add ASR2 crank timestamp: + var_asr2_time + ne_sum3 - 0x35
  │
  ├─ ignition_schedule_off → CPR0 = fire_time, DOM DOUT.0=1
  └─ ignition_schedule_on  → CPR0 = charge_time, DOM DOUT.0=0

int_vector_9_ignition  [fires at each CPR0 match]
  │
  ├─ Coil just fired → check IGF, schedule next charge via ignition_schedule_on
  ├─ Coil just charged → call ignition_update_off_time to refine dwell
  └─ No schedule ready → fixed 65ms emergency dwell (CPR0 += 16250)

IGF monitoring
  IRQLL.4 (ASR0 edge) → if missing:
    var_igf_miss_count++ → ≥5 → FUEL CUT (var_limiter_flags.4)
    igf_error_count++   → ≥9 → var_error_flags1.2 (IGF error flag)
```

---

## Variable Reference

| Variable | Description |
|---|---|
| `var_ign_timing_div_2` | Current ignition advance (0.5°/count, 0x2B = 0°BTDC) |
| `var_ign_advance_raw` | Raw computed advance (before dwell subtraction) |
| `var_ign_advance_trim` | Additional trim added to final timing |
| `var_ign_advance_max` | Maximum allowed advance (upper clamp) |
| `var_ign_timing_min` | Minimum allowed advance (lower clamp) |
| `var_ign_cold_advance` | Fixed advance value for cold start / limp mode |
| `var_ign_knock_retard_base` | Knock retard base value (subtracted from advance) |
| `var_ign_dwell_offset` | Dwell endpoint offset for coil inductance compensation |
| `var_ign_dwell_min` | Minimum dwell time (prevents very short dwell at high RPM) |
| `var_ign_nr_pulses` | Number of NE pulses for multi-pulse timing calculation |
| `var_ign_temp` | Temporary scratch variable used during timing computation |
| `var_ign_coil_on_time` | CPR0 value when coil last turned on (for dwell feedback) |
| `var_ign_next_cpr` | Requested next CPR0 event time |
| `var_ign_ne_frac` | Fractional NE period for sub-degree timing resolution |
| `var_igf_miss_count` | Count of consecutive missing IGF signals (max 0xFF) |
| `var_igt_timer` | IGT active timer (detects stuck-on ignition) |
| `var_ignition_flags` | Ignition state flags (see below) |
| `dmarx_ign_advance_hi` | RPM advance from CPU2 (high RPM band) |
| `dmarx_ign_advance_lo` | RPM advance from CPU2 (low RPM band) |
| `dmatx_knock_retard` | Knock retard command (from CPU1 knock system, sent to CPU2) |
| `dmatx_ign_obd` | Ignition timing in OBD1 format (sent to CPU2 for diagnostics) |

### `var_ignition_flags` Bit Assignments

| Bit | Meaning |
|---|---|
| 0 | Ignition off-time (fire event) has been scheduled |
| 1 | Ignition on-time (charge event) has been scheduled |
| 2 | Limp/fault mode: use fixed timing, disable normal advance |
| 3 | Ignition sequence in progress |
| 4 | Fixed dwell timer active |
| 5 | Pending on-time: on-time was set during an active fire event |

### Error Flags

| Flag | Condition | Meaning |
|---|---|---|
| `var_limiter_flags.4` | IGF miss count ≥ 5 | Fuel cut due to missing IGF |
| `var_error_flags1.2` | IGF miss count ≥ 9 AND timer expired | IGF missing diagnostic error |
| `var_flags_4D.4` | IGF error latched | Too many IGF signals missing |

---

## Fault Modes and Override Conditions

| Fault | Timing override | Note |
|---|---|---|
| `var_flags_46.7` (knock sensor error) | −5° BTDC fixed | Severe knock fault |
| `var_flags_4D.3` (default PIM value) | −5° BTDC fixed | MAP sensor fault |
| `var_flags_46.2` AND diagnostic mode | −10° BTDC fixed | Test mode |
| `var_ignition_flags.2` (limp mode) | `var_ign_cold_advance` | ECU fault / cold cranking |
| IGF missing ≥ 5 sparks | Fuel cut | Misfire protection |

---

*Derived from IDA disassembly of D151803-9651 (CPU1), Toyota 3S-GTE ECU.*
