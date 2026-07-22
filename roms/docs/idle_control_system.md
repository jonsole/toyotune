# Idle Speed Control (ISCV) System — 3S-GTE ECU CPU1 (D151803-9651)

## Overview

The idle speed control (ISCV) system computes a duty-cycle target for the idle
speed control valve solenoid every 4ms and drives it through a timer-compare
PWM output, the same scheduling pattern used for ignition (CPR0) and
injection (CPR4/6/7/5).

Two functions cooperate:

- **`calc_iscv`** — the target computation. Reads RPM error, coolant/intake
  temperature, throttle/idle switch state, battery voltage headroom, and
  learned NV RAM trim, and produces a single duty value in `var_iscv_19D`.
- **`drive_dout1_iscv`** — the hardware driver, called every 4ms from
  `int_4ms_watchdog`. Combines `var_iscv_19D` with battery dead-time
  compensation (`calc_idle_batt1`/`calc_idle_batt2`, computed in
  `divide_d_by_x`) into `var_iscv_pwm`, then arms `CPR1` and `DOUT.1` to
  produce the actual pulse.

**ISCV driver (DOUT.1 / CPR1):**
- `var_iscv_pwm == 0` → valve pulse skipped entirely this cycle
- Otherwise: `DOUT.1 = 1`, `CPR1 = TIMER + var_iscv_pwm` (pulse ends at the
  next CPR1 match, `DOM.1` latched to track the pending event)

`calc_iscv` itself is only called when `unk_44.1` is clear (gate at
`divide_d_by_x:loc_D380`) — the exact condition this bit tracks was not
pinned down this session.

---

## Fixed-Opening Override (bypasses `calc_iscv` entirely)

`var_iscv_19D` (calc_iscv's computed target) is **not always used**. The code
that runs immediately after `calc_iscv` each 4ms tick (`divide_d_by_x`,
chunk @ `D3A5`) can substitute a fixed raw pulse width - `0x0200` or `0x0300`
timer units - instead, bypassing `var_iscv_19D` completely on the way to
`var_iscv_pwm`. This happens during the startup window (`var_cnt_C7 < 0x3D`)
or when any of several "not ready" conditions are flagged: `var_flags_46.6`,
`var_flags_46.7`, `var_flags_4F.5`, or the `var_4ms_cnt_B6`/`B7` debounce
timers not having elapsed. Only once none of these apply does control reach
the battery-compensation + `var_iscv_19D` combination described above.

This is the evidence behind `var_flags_46.6`'s meaning: while set, the ISCV
runs in this fixed-override mode rather than `calc_iscv`'s closed-loop
target. The bit is set/cleared by a debounce chain in `divide_d_by_x`
(chunk @ `C9DA`) gated on `var_io_input1` bits 2/3 (both otherwise
undocumented signals) and startup timing - so "not yet settled after start"
turned out to be a reasonable characterization, just driven by specific
input-pin debouncing rather than a generic readiness flag.

---

## Learned Idle Trim (NV RAM)

Idle position is adaptively trimmed and persisted the same way AFR trim is
(see `nv_afr_trim_base` in the ignition/fuel docs): a running `var_idle_trim`
value is nudged toward a target and written back to NV RAM
(`var_nv_idle_trim`) via `write_rB_nv_ram` once the idle condition has held
stable for long enough (`var_cnt_DD`/`var_cnt_DE`, ~368ms thresholds). If
`var_flags_42.0` (idle trim valid) is clear, a default of `0x66` is used
instead of the NV value throughout.

This learned trim is the baseline every other computation in `calc_iscv`
scales from (`nv_idle_trim * 16` appears repeatedly as the "cold" baseline).

---

## `calc_iscv` — Structure

The function is split into two IDA chunks (`D4E4`..`D602` and `D612`..`D92D`)
and internally into six phases. See the header comment on `calc_iscv` in
`D151803-9651.asm` for the address-referenced version of this breakdown;
this is the narrative form.

### Phase 1 — Flare/enrichment terms → target idle RPM

Several independent terms are computed, each representing "extra idle air"
needed for a specific condition, then summed into a single **target idle
RPM** (`var_iscv_target_rpm`, in `var_rpm_x_5p12` units):

| Term | Behaviour |
|---|---|
| `var_iscv_startup_flare` | Decays -1/tick once the startup window (`var_cnt_startup` ≥ 0x3D ≈ 244ms) has passed; before that, held at whatever `max(ECT, THA)/16` was on entry |
| `var_iscv_pim_flare` | Set from a PIM-indexed table on throttle lift-off/deceleration (`var_flags_4E.4`, gated on RPM > 2000, speed < 5kph, small RPM delta); decays -8/tick otherwise |
| `unk_1A9` | Fixed at `0x300` during the startup window, then decays -4/tick |
| `unk_1AB` | `0x200` for the first 15 ticks if CPU2 cold-enrichment (`dmarx_idle_enrich`) is active, else cleared once `var_cnt_EA` elapses |
| `unk_1AD` | Ramps ±2/tick toward a load-dependent set-point (see below) |

`unk_1AD`'s set-point is selected from `byte_C372`/`byte_C374` based on
`var_flags_4F.1`, further offset via `inc_rX_if` (gated on `var_flags_4F`
bits 2/3). **Hypothesis (unconfirmed):** `var_flags_4F` bits 1-4 consolidate
debounced Air-Con (`var_diag_errors_5.5`) and PS/IDUP (`var_io_input2.3`)
switch state specifically for idle-up compensation, since those are the only
two "extra electrical/mechanical load" switches documented elsewhere in the
ROM. The raw bits were not traced back to their source this session.

A separate threshold check (`byte_C36C`/`C36E`/`C370`, also switch-selected)
sets `var_diag_errors_5.0` and feeds both `check_knock_sensor_err_flag` and
an accumulator `unk_1A7` — purpose not fully confirmed, but it participates
in the Phase 1 sum and later resurfaces in Phase 3.

The five terms are summed, plus a `table_iscv_C391` entry (values `0x00, 0x08,
0x10, 0x20`) selected by `var_io_input2` bits 6/7 (undocumented elsewhere in
the ROM — two more load-switch bits, meaning not confirmed), giving
`var_iscv_target_rpm`. The actual/target RPM difference,
`(var_rpm_x_5p12 - var_iscv_target_rpm) / 16` saturated, becomes
**`var_iscv_rpm_cmp_197`** — the idle RPM error term consumed everywhere
downstream.

### Phase 2 — `var_iscv_target_base` update

`var_iscv_target_base` is the persistent baseline duty level. Two paths:

- **At stable idle** (`var_cnt_DB` elapsed, idle flag set, no fault flag):
  search `table_iscv_rpm_C357` for the band containing `var_iscv_rpm_cmp_197`
  (ascending linear scan, 5-byte-stride entries — a step value at `entry+5`),
  add the step to the *current* `var_iscv_target_base`, bias-center by
  subtracting `0x80`.
- **Otherwise:** candidate is simply `nv_idle_trim * 16`, ratcheted up only
  (never down) against the current baseline.

A second computation (always run) derives a ceiling from `nv_idle_trim`
offset by a P/N-switch-selected range (`var_flags_4F.2`: `+0x148` or
`-0x33+0x17B`). **Correction:** this was originally described as a clean
"clamp the RPM-band candidate to this ceiling" — re-examined and that's not
quite right. The `mov` instruction is **src, dest** (opposite of `ld`/`st`,
easy to misread — see `fuel_calculation_system.md`'s "mov direction" note),
which changes the register tracking here: when the ceiling exceeds the
band candidate, the code re-derives from a value stashed *before* the
ceiling's final offset was added (`nv_idle_trim*16` or `nv_idle_trim*16-0x33`,
depending on the P/N-switch branch), not from the ceiling itself. The final
result is then either that stashed value or the band candidate — the
ceiling as computed is never actually used in that branch. Net effect on
`var_iscv_target_base` not fully understood beyond this corrected
mechanical trace; flagged as an open question below.

### Phase 3 — ECT and secondary RPM-band terms

- `unk_1A0`: ECT-indexed 4-entry lookup (`byte_C352`/`C354` via
  `table_ect_fixed4_interpolate`), refined against `unk_1A7` via
  `interp_y_pair`.
- `unk_1A1`: mirrors Phase 2's RPM-band search but against
  `table_iscv_rpm_C361` (same 5-byte-stride/step-value layout as `C357`),
  producing a second running value. Below the `0xA66` candidate threshold
  the new value passes straight through; above it, the step from the
  previous `unk_1A1` is rate-limited to `±0x400` per tick.

### Phase 4 — Idle trim learning

Once idle has been stable long enough (`var_cnt_DD`/`var_cnt_DE`, ~368ms):
`var_idle_trim` is nudged via `idle_trim_limits` (min/max clamp table,
`0x78`/`0x45`) and written back to NV RAM. Conditions gating the nudge
include ECT, battery voltage, fuel/idle DMA trim state, and whether recent
flare terms (`unk_1AB`, `var_iscv_startup_flare`) are still active — i.e.
the trim is only allowed to adapt once the engine is warm, stable, and not
mid-flare. `unk_9E` appears to record which direction/validity state the
last nudge left the system in, but wasn't fully traced.

### Phase 5 — RPM-slope terms

- `unk_1A5`: a first-order low-pass filter tracking `var_rpm_x_5p12`,
  stepping 1/4 of the way to the current RPM each call (smoothed RPM
  reference, via `divide_rD_4_signed`).
- `unk_1A3`: derived from the gap between current RPM and `unk_1A5` under a
  low-speed gate (`var_speed_kph < 2`), doubled/saturated and generally
  clamped to `0x400` — reads as a stall-recovery/derivative term, though the
  exact RPM-band thresholds (`0x14`, `0x0D`) weren't fully resolved.

### Phase 6 — Final map lookup → `var_iscv_19D`

The primary 2D interpolation: `table_idle_C2FE` via `map_rD_rX_interpolate`
(bilinear — the same helper used for fuel/ignition maps), indexed by
`var_iscv_rpm_cmp_197`-derived and load-flag-derived axes. The result is
clamped to a max of `0x50` (`var_iscv_unk_19F`), then combined with
`unk_1A0`, `unk_1A1`, `var_iscv_startup_flare`/`var_iscv_target_base`
(selected by `var_flags_46.6` — see Open Questions), and a `table_unk_C34A`
load-compensation entry (selected by A/C-adjacent flags and
`var_flags_42.6`). A final ECT/RPM-band gate (`var_cnt_DC`, 2800/3000rpm
thresholds) chooses between this computed value and a `table_rpm_c31d`
fallback, also toggling `var_flags_4E.3` and `var_flags_4F.6`. The result is
stored to `var_iscv_19D`.

---

## Data Flow Diagram

```
calc_iscv  [4ms tick, gated on unk_44.1 clear]
  │
  ├─ Phase 1: flare/enrichment terms -> var_iscv_target_rpm
  │            (var_rpm_x_5p12 - target_rpm) -> var_iscv_rpm_cmp_197
  ├─ Phase 2: RPM-band table (table_iscv_rpm_C357) or nv_idle_trim fallback
  │            -> var_iscv_target_base (ratcheted baseline)
  ├─ Phase 3: ECT term (unk_1A0) + second RPM-band table (table_iscv_rpm_C361)
  │            -> unk_1A1 (rate-limited)
  ├─ Phase 4: idle trim learning -> var_idle_trim -> write_rB_nv_ram
  │            -> var_nv_idle_trim (persisted)
  ├─ Phase 5: RPM-slope terms (unk_1A5, unk_1A3)
  └─ Phase 6: table_idle_C2FE bilinear map + load compensation
               -> var_iscv_19D

divide_d_by_x (loc_D44B, still in the 4ms tick)
  │
  ├─ calc_idle_batt1 / calc_idle_batt2 (battery dead-time compensation)
  ├─ combine with var_iscv_19D
  └─ var_iscv_pwm

drive_dout1_iscv  [4ms tick, from int_4ms_watchdog]
  │
  ├─ var_iscv_pwm == 0 -> skip pulse this cycle
  └─ else: DOUT.1 = 1, CPR1 = TIMER + var_iscv_pwm, DOM.1 latched
```

---

## Variable Reference

| Variable | Description |
|---|---|
| `var_iscv_target_rpm` | Target idle RPM, in `var_rpm_x_5p12` units (sum of flare/enrichment terms + load compensation) |
| `var_iscv_rpm_cmp_197` | Idle RPM error: `(var_rpm_x_5p12 - var_iscv_target_rpm) / 16`, saturated |
| `var_iscv_target_base` | Persistent ratcheted duty baseline, seeded from `nv_idle_trim * 16 + 0xA4` |
| `var_iscv_startup_flare` | Extra idle air during/just after startup, ECT/THA or decay-based |
| `var_iscv_pim_flare` | Extra idle air on throttle lift-off/deceleration, PIM-indexed |
| `var_iscv_unk_19F` | Clamped (max 0x50) intermediate from the Phase 6 map result |
| `var_idle_trim` | Learned idle trim, adapted toward a target when idle is stable |
| `var_nv_idle_trim` | `var_idle_trim` persisted in battery-backed NV RAM |
| `var_iscv_19D` | Final duty target from `calc_iscv`, consumed by `divide_d_by_x` |
| `var_iscv_pwm` | Final PWM pulse width (timer units) consumed by `drive_dout1_iscv` |
| `var_iscv_ect_unk_191` | ECT-indexed idle baseline computed by the separate `calc_ect_iscv` helper |
| `unk_1A0` | ECT-indexed correction term (Phase 3) |
| `unk_1A1` | Secondary RPM-band running value, rate-limited (Phase 3) |
| `unk_1A3` | Stall-recovery/derivative-like term from RPM slope (Phase 5) |
| `unk_1A5` | Low-pass filtered ("smoothed") RPM reference (Phase 5) |
| `unk_1A7` | Diagnostic-linked accumulator from the `byte_C36C` threshold check (Phase 1), reused in Phase 3 |
| `unk_1A9` | Post-start decaying flare term (Phase 1) |
| `var_iscv_unk_1AB` | Cold/CPU2-enrichment-linked flare term (Phase 1) |
| `var_iscv_unk_1AD` | AC/PS-load-dependent ramp term (Phase 1, hypothesis) |
| `unk_9E` | Idle trim nudge direction/validity state (Phase 4, not fully confirmed) |
| `unk_E2` | Cleared alongside idle-trim-stability resets; role not confirmed |

### Key Tables

| Table | Indexed by | Used for |
|---|---|---|
| `table_ect_idle_flare` | ECT | Phase 1 startup flare (ECT branch) |
| `table_tha_idle_flare` | THA | Phase 1 startup flare (THA branch) |
| `table_idle_pim` | PIM | Phase 1 PIM flare |
| `table_iscv_C391` | `var_io_input2` bits 6/7 (4 combinations) | Phase 1 load compensation, added to target RPM |
| `table_iscv_rpm_C357` | `var_iscv_rpm_cmp_197` (ascending band search) | Phase 2 `var_iscv_target_base` step |
| `table_iscv_rpm_C361` | `var_iscv_rpm_cmp_197` (ascending band search) | Phase 3 `unk_1A1` step |
| `byte_C352`/`byte_C354` | ECT (`table_ect_fixed4_interpolate`) | Phase 3 `unk_1A0` |
| `idle_trim_limits` | — (min/max pair: `0x78`/`0x45`) | Phase 4 clamp for `var_idle_trim` nudges |
| `table_idle_C2FE` | RPM error + load (bilinear, `map_rD_rX_interpolate`) | Phase 6 primary duty map |
| `table_unk_C34A` | A/C-adjacent flags + `var_flags_42.6` (4-way select) | Phase 6 load compensation |
| `table_rpm_c31d` | RPM (`var_rpm_div_25`) | Phase 6 fallback path |
| `table_ect_idle_C323` | ECT | `calc_ect_iscv` (separate helper, feeds `var_iscv_ect_unk_191`) |

---

## Open Questions (not resolved this session)

- `unk_44.1`'s exact meaning — it gates whether `calc_iscv` runs at all this
  tick, and is cleared once per NE cycle at the TDC-ish position, but its
  setter wasn't located.
- ~~`var_flags_46.6`~~ **Resolved** (see "Fixed-Opening Override" above): it
  gates whether the ISCV runs `calc_iscv`'s closed-loop target or a fixed
  override pulse, and is debounced from `var_io_input1` bits 2/3 plus
  startup timing in `divide_d_by_x` chunk `C9DA`. The two undocumented
  `var_io_input1` bits driving it remain unidentified.
- `var_io_input2` bits 6/7 — feed `table_iscv_C391`'s load-compensation
  selection but have no documented signal name (unlike bit 0 = ECO, bit 3 =
  PS/IDUP).
- `unk_1A7`, `unk_9E`, `unk_E2` — participate in the flare/trim logic but
  their precise roles weren't pinned down; left unrenamed rather than
  guessed at.
- `table_iscv_rpm_C357`/`C361`'s exact byte layout (why a 5-byte stride, what
  the other 4 bytes per entry hold beyond the one step value read) wasn't
  fully reverse-engineered.
- Phase 2's ceiling-vs-band-candidate logic (`loc_D651`-`loc_D67C`): after
  correcting the `mov` direction, the ceiling computed there is discarded
  in the branch where it would matter (ceiling > band candidate), and a
  stashed pre-offset value is compared against the band candidate instead.
  Why the code is written this way - whether it's intentional or the
  ceiling computation is dead weight left over from a refactor - isn't
  understood.

---

*Derived from IDA disassembly of D151803-9651 (CPU1), Toyota 3S-GTE ECU.*
