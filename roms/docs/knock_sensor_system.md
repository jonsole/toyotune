# Knock Sensor System — 3S-GTE ECU (D151803-9651 / D151803-9661)

## Overview

The knock control system spans both CPUs and uses a dedicated knock detection MCU. It implements **per-cylinder adaptive knock retard** that is learned and persisted across ignition cycles via battery-backed PRAM.

The system has three layers:

1. **Hardware** — a dedicated knock MCU (SDIP64 D8X) connected to the piezo knock sensors, communicating knock level to CPU1 via PORTB
2. **CPU1** — reads knock level every NE pulse, integrates per-cylinder retard, stores learned values in PRAM, and transmits to CPU2 via DMA
3. **CPU2** — receives knock retard from CPU1 and applies it to the ignition timing calculation

---

## Hardware Interface

The knock MCU communicates with CPU1 via a bit-banged serial protocol over PORTB, timed to NE crank pulses.

### PORTB Pin Assignments

| Pin | Direction | Function |
|---|---|---|
| PORTB.0 | Output | Clock / handshake strobe to knock MCU |
| PORTB.1 | Output | TDC cylinder 1 reference signal to knock MCU |
| PORTB.3 | Input | Knock level bit 0 (LSB) — active low |
| PORTB.4 | Input | Knock level bit 1 — active low |
| PORTB.5 | Input | Knock level bit 2 (MSB) — active high |

### DOUT.2

DOUT.2 is the knock MCU hardware reset line. It is normally held high. Pulsing it low for approximately 12 µs (3 × divide-by-zero NOP delay) resets the knock MCU.

---

## var_knock_info — 3-bit Knock Level Accumulator

`var_knock_info` is a 1-byte variable that accumulates the 3-bit knock level from the knock MCU over two NE positions, then is decoded at the third position.

### Bit Encoding

| Bit | Source | Meaning |
|---|---|---|
| 0 | PORTB.3 low | Knock level LSB |
| 1 | PORTB.4 low | Knock level mid |
| 3 | PORTB.5 high | Knock level MSB |

Note: bits are active-inverted for bits 0 and 1 (set when PORTB pin is **low**), but active-true for bit 3 (set when PORTB.5 is **high**).

---

## `knock_mcu_update` — Called Every NE Pulse (24× per Revolution)

This function is called from `int_vector_e_ne` on every NE pulse and performs different actions depending on the crank position counter (`var_ne_count` bits 2..0):

### Position 0 — Transmit Reference + Read Knock Data

**Transmit:**
- If `ne_count == 0x00` (TDC cylinder 1): assert `PORTB.1` high
- Other cylinders at position 0: clear `PORTB.1`

**Read knock data** (all positions 0):
- Read `PORTB.5` → `var_knock_info` bit 3
- Strobe `PORTB.0` low
- Read `PORTB.4` (active low) → `var_knock_info` bit 1
- Read `PORTB.3` (active low) → `var_knock_info` bit 0

### Position 1 — Handshake Pulse

Send a handshake pulse to the knock MCU: `PORTB.1` low, then `PORTB.0` high. No knock data read.

### Position 2 — Decode and Process Knock Level

This is the main processing step. Several gate conditions must all be satisfied before processing:

| Gate condition | Skip if… |
|---|---|
| Init guard flag | `var_flags_40.5` is set |
| Knock MCU reset | `DOUT.2` is low (MCU in reset) |
| Interrupt pending | ASR0 interrupt pending |
| ASR3 pin | ASR3 input is high |
| Engine settle | 4ms counter `var_4ms_cnt_BE` < 0x7A |
| Post-start | starter counter `var_4ms_starter_cnt_C0` < 0x7A |

If all gates pass, the accumulated `var_knock_info` is decoded:

#### Knock Level Decode

```
B = var_knock_info
if PORTB.5 high: B += 4    (add bit2 of level)
B >>= 3                     (3 logical right shifts)
rorc B                      (rotate right through carry)
```

The resulting V and C flags determine the knock state:

| V flag | C flag | Meaning |
|---|---|---|
| Clear | — | Knock detected |
| Set | Set | No knock |
| Set | Clear | Borderline / suspicious signal |

#### No Knock (V set, C set)

If `var_error_flags2.0` (knock error) is not latched:
- Clear `var_flags_46.5` (knock sensor error)
- Clear `var_diag_errors_5.4` (knock management diagnostic error)
- Clear `var_cnt_knock_signal`

`var_knock_info` is reset to 0 for the next accumulation cycle.

#### Borderline Knock (V set, C clear)

Valid RPM window: **2850–7200 RPM**. Outside this window, the signal is ignored.

- If `var_cnt_knock_signal < 3`: increment counter and skip
- If `var_cnt_knock_signal >= 3` and `var_diag_errors_5.7` (G1/G2 error) is set:
  - Set `var_flags_46.5` (knock sensor error)
  - Set `var_error_flags2.0` (knock sensor diagnostic error)

#### Knock Detected (V clear)

Valid RPM window: **700–7200 RPM**. Also requires `nv_diag_errors_1 == 0x80` (no G1/G2 NV error, i.e. crank sync is reliable).

The cylinder knock counter (upper nibble of A) is incremented by 0x10 each time knock is detected:

- If counter reaches **0xC0** (12 cylinders): set `var_diag_errors_5.4`, reset counter, reset knock MCU
- If counter reaches **0x60** (6 cylinders): reset knock MCU

### Knock MCU Reset

```asm
clrb bit2, DOUT         ; Assert reset (low)
div  d, #00h            ; ~4 µs delay
div  d, #00h            ; ~4 µs delay
div  d, #00h            ; ~4 µs delay
setb bit2, DOUT         ; Release reset (high)
setb bit5, var_flags_46 ; Set knock sensor error flag
```

### Positions 3..5

No action. Return immediately.

---

## `knock_processing` — Per-Cylinder Retard Integration

Called from `iv6_ne_process` at NE position 0, cylinder 0 (once per revolution). This is where the raw knock signal from `var_knock_info` is translated into a per-cylinder ignition retard value.

### Knock Retard Step Table (`table_knock_retard_step`)

```
table_knock_retard_step: .db 0x02, 0x04, 0x06
```

Indexed by `var_knock_info bits 1..0` (knock level 1, 2, or 3). These are the retard step sizes to add to `var_knock_retard` on each knock event. Units are approximately 0.5° per count (i.e. levels of 1°, 2°, and 3°).

### Retard Increment Table (`table_knock_retard_inc`)

```
table_knock_retard_inc: .db 0x01, 0x01, 0x02
```

Indexed by `var_knock_cyl_idx` (0..2). Scales the retard increment for different RPM bands.

### RPM Band Table (`table_knock_rpm_bands`)

```
table_knock_rpm_bands:
  .db 0x70, 0xC0    ; 2800–4800 RPM  -> cylinder group 0
  .db 0x68, 0xC0    ; 2600–4800 RPM  -> cylinder group 1
  .db 0x68, 0xB8    ; 2600–4600 RPM  -> cylinder group 2
```

Each pair is `[lower_threshold, upper_threshold]` in `var_rpm_div_25` units (RPM/25). `var_knock_cyl_idx` selects which group is active based on current RPM.

### Integration Logic

On each NE cycle:

1. Look up retard step from `table_knock_retard_step[knock_level]`
2. If cold start conditions apply (ECT low): double the step
3. If very recent start (counter check): double again
4. Add step to `var_knock_retard`, saturate at 0xFF
5. If `var_knock_retard > var_knock_retard_max`: clamp and update `var_knock_retard_max`
6. Decay path: compare against `var_knock_retard_prev` and `dmarx_knock_retard_cpu2` (CPU2 contribution)
7. Clamp final value at 0x1A (26 counts ≈ 13°)
8. Store to `nv_table_knock_info[var_knock_cyl_idx]`

### `knock_retard_decay` — Called Every 4ms

Decays `var_knock_retard` by 2 counts each 4ms period (allowing retard to recover when knock clears). If knock error flag is set, `nv_table_knock_info` is reset to `0x9A9A` (both bytes). Also computes `dmatx_ign_corr_cpu2` (the retard command sent to CPU2, at 0x216 - the doc previously called it `dmatx_unk_216` after its address) from the ECT-corrected retard value and `nv_table_knock_info`.

---

## Retard response is front-loaded (`loc_F57C`)

The base step comes from `table_knock_retard_step`, indexed by the low two
bits of `var_knock_info`. Note it is reached as `table_knock_retard_step-1`
plus the level, so **level 0 is not a valid entry**.

That base is then scaled by how many events have already occurred:

| `var_knock_event_cnt` | step |
|---|---|
| > 4 | base step, unchanged |
| <= 4 | **doubled** |
| <= 1 | a further branch applies |

So the **first** sign of knock gets the largest retard and repeated events
trim progressively more finely — retard hard, then converge.
`dmatx_ign_corr_cpu2` is consulted on the way through, so CPU2's own
correction participates in the decision.

The per-cylinder increment at `loc_F661` is separate: `table_knock_retard_inc`
indexed by `var_knock_cyl_idx`, added to the previous value and clamped at
`0xAB`, and only applied inside an `0x18`..`0x8C` RPM window.

**Decay is fixed-rate.** `var_cnt_knock_decay` reaches `0x40` every 256 ms
and only then is retard reduced by 2 — so recovery proceeds at 2 counts per
256 ms regardless of how often `knock_retard_decay` is called.

---

## Names that look like knock but are not

Several symbols in this ROM carry "knock" in their names purely as an
artefact of early guesswork, and reading them as knock data leads directly
to wrong conclusions. Recording them here because the trap has caught this
effort more than once.

**`set_knock_sensor_err_flag` / `check_knock_sensor_err_flag` are a generic
abs() idiom.** The pair works by deliberate fall-through into `negate_rD`:

- `set_knock_sensor_err_flag` sets `var_diag_errors_5.0` **and**
  unconditionally negates `D`;
- `check_knock_sensor_err_flag` negates `D` only if that flag is set.

The pattern at each call site is: compute a difference that may have
underflowed, call `set_` to take its magnitude while remembering the flip,
do further arithmetic on the positive value, then call `check_` to restore
the sign. **`var_diag_errors_5.0` is a "did we negate" remember-bit**, not a
knock-sensor fault. It is genuinely knock-related only at knock-subsystem
call sites such as `knock_mcu_update`; in `calc_iscv`, `ramp_limit_inj_pw`
and `update_ign_timing_blend` it is borrowed purely for the negate.

**Variables renamed once this was understood:**

| old name | now | what it actually is |
|---|---|---|
| `var_unk_knk_133`/`135` | `var_pim_est_fast`/`slow` | manifold-pressure filter pair |
| `var_unk_knock_12B` | `var_ign_blend_hist0` | ignition-blend delay line |
| `knock_unk_E79E` | `update_pim_est_fast` | MAP estimate filter |
| `some_knock_averaging_calc` | `update_pim_est_slow` | its slower stage |
| `sub_E551` | `calc_dmatx_pim` | MAP transient compensation |

`sub_E551` is the cautionary one: it was believed for several sessions to be
a knock/boost limiter, purely because it called the two functions above. Its
single exit stores to `dmatx_pim` — it is manifold-pressure transient
compensation and has nothing to do with knock.

**Genuinely knock-related and confirmed:** `var_knock_retard`,
`var_knock_retard_max`, `var_knock_retard_prev`/`prev2`,
`var_knock_cyl_idx`, `var_knock_event_cnt`, `var_knock_info`,
`var_g1g2_err_cnt`, and `var_knock_retard_latch` — the last being a snapshot
of `var_knock_retard` taken only when persistent knock coincides with a
G1/G2 sync error.

**One dead gate:** `var_knock_gate_168` is only ever cleared and has no
setter — verified against both indirect-write mechanisms — so the `bne` at
`loc_F5F4` never branches and control always reaches the
`var_knock_retard_max` comparison. A knock path disabled by zeroing rather
than removed.

---

## Key Variables

| Variable | Address | Description |
|---|---|---|
| `var_knock_info` | RAM | 3-bit knock level from knock MCU, accumulated per NE cycle |
| `var_knock_retard` | RAM | Per-cylinder retard integrator (counts up on knock, decays 2/4ms) |
| `var_knock_event_cnt` | RAM | Knock event accumulation counter within current cycle |
| `var_knock_retard_prev` | RAM | Previous cycle retard value (for rate limiting) |
| `var_knock_retard_max` | RAM | Maximum retard reached this cycle |
| `var_knock_retard_prev2` | RAM | Second previous retard value |
| `var_knock_cyl_idx` | RAM | Active cylinder group index (0..2), selected by RPM |
| `var_cnt_knock_signal` | RAM | Count of consecutive knock signals (threshold = 3) |
| `nv_table_knock_info[3]` | PRAM | Per-cylinder learned knock retard (persists over ignition off) |
| `dmatx_knock_info[3]` | DMA TX | Copy of `nv_table_knock_info` sent to CPU2 every 4ms |
| `dmarx_knock_retard_cpu2` | DMA RX | Knock retard contribution received from CPU2 |

---

## Data Flow Diagram

```
Knock MCU (hardware)
  │  PORTB.3 / .4 / .5 (knock level bits, read at NE positions 0 & 1)
  ▼
var_knock_info  [1 byte, 3-bit level accumulator]
  │  Decoded at NE position 2 via 3×shr + rorc
  ▼
V/C flag decode ──────────────────────────────────────────────────────────┐
  │ V clear (knock)         │ V set, C set (no knock)  │ V set, C clr    │
  ▼                         ▼                          ▼                 │
table_knock_retard_step   Clear error flags       Accumulate counter      │
  │ [0x02, 0x04, 0x06]                            (max 3 events)         │
  │ (step size by level)                               │ >= 3 + G1/G2    │
  ▼                                                   ▼ error            │
var_knock_retard [integrator]              var_error_flags2.0             │
  │  + step on knock                      var_flags_46.5                  │
  │  - 2 per 4ms (decay)                                                 │
  │  clamped 0..0x1A (0..~13°)                                           │
  │                                                                       │
  ▼                                  Cylinder counter >= 0x60 or 0xC0 ───┘
nv_table_knock_info[3]  [PRAM]                    │
  │  Per-cylinder learned retard                  ▼
  │  Indexed by var_knock_cyl_idx          knock_mcu_reset
  │  (RPM-selected cylinder group)         (DOUT.2 pulse ~12µs)
  │  Persists over ignition off
  ▼
dmatx_knock_info[3]  [DMA TX buffer]
  │  Sent to CPU2 every 4ms
  ▼
CPU2 → ignition timing calculation → dmatx_ign_timing → back to CPU1
```

---

## RPM Windows

| Window | Lower | Upper | Used for |
|---|---|---|---|
| Knock detection (V clear) | 700 RPM | 7200 RPM | Full knock retard integration |
| Borderline signal (V set, C clear) | 2850 RPM | 7200 RPM | Error accumulation only |
| Decay active | 600 RPM | 5600 RPM | `knock_retard_decay` active band |

---

## Error Flags

| Flag | Meaning |
|---|---|
| `var_flags_46.5` | Knock sensor error (operational) |
| `var_error_flags2.0` | Knock sensor diagnostic error (latched) |
| `var_diag_errors_5.4` | Knock management diagnostic error |
| `var_diag_errors_5.7` | G1/G2 signal error (invalidates knock data) |
| `nv_diag_errors_1` | NV knock error (must equal 0x80 for knock data to be valid) |

---

*Derived from IDA disassembly of D151803-9651 (CPU1) and D151803-9661 (CPU2), Toyota 3S-GTE ECU.*
