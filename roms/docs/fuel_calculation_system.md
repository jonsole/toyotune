# Fuel Pulse-Width Calculation — 3S-GTE ECU CPU1 (D151803-9651)

## Overview

This covers the core base injector pulse-width calculation (`divide_d_by_x`
chunk @ `D931` and its continuation), upstream of the per-injector
dead-time/battery-voltage driver logic already documented in the journal's
"Injector system" section (`injector_drive`, `injectors_batch_update`, etc.).
Each 4ms tick, this code combines CPU2's VE/fuel-map DMA output with the
closed-loop lambda integrator to produce a working pulse-width value,
`var_inj_pw_base`, through an open-loop/closed-loop path selection and a
rate-limited blending step.

---

## Critical: `mov` operand direction

**The `mov` instruction is `mov src, dest` — the opposite of `ld`/`st`,
which are `dest, src`.** This is confirmed two independent ways: (1) the
technical reference's instruction table states `mov x, d` = "D ← X" and
`mov d, x` = "X ← D" explicitly; (2) the gold-standard `knock_mcu_update.ASM`
reference annotates `mov a, b` as "B = var_knock_info" where A held that
value, matching the table exactly; (3) `divide_d_by_x`'s own well-established
implementation only makes sense with this direction (e.g. its first `mov x, d`
must move the divisor from X into D for the normalization loop that follows,
not the reverse).

This is very easy to misread — it was misread in this session's own
`calc_iscv` comments (now corrected) before being caught while re-examining
the VE-map candidate calculation below. **Pre-existing comments elsewhere in
this ROM's disassembly (from earlier sessions) also show a consistent
reversed-notation pattern** — e.g. `mov s, x` is commented "SP = X" in
several places in the shared math library (`table_rB_fixed_rA_interpolate`
and others), when the correct reading is "X = SP(+1)". These weren't
corrected this session (out of scope - a dedicated pass would be needed to
find and fix every instance across the ROM), but any future work should
double-check `mov` direction rather than trusting an existing comment at
face value.

---

## Critical: the `var_flags_4E` / `var_trim_state` alias

**Read this before touching any code in the range below, or misreading
`var_flags_4E` bit-tests is inevitable.**

For roughly the address range `0xD931`-`0xE380` (chunk `D931`, chunk `DC77`,
and several chunks after it, up to a restore point in chunk `E363`),
`var_flags_4E` is deliberately overwritten to hold `var_trim_state`'s value
and used purely as a scratch register, so the existing `tbbc`/`tbbs`/`setb`/
`clrb` instruction encodings (which hardcode `var_flags_4E` as their operand)
can be reused against `var_trim_state`'s bits instead of the assembler
emitting separate code for each. Confirmed by:

- Entry (`calc_inj_pw_base`): the *real* `var_flags_4E` is snapshotted to
  `var_flags_4E_copy2` and `var_flags_4F` to `unk_1D8`, then
  `var_flags_4E` is overwritten with `var_trim_state`.
- Exit (`loc_DC77`): `var_flags_4E`'s current value is written back to
  `var_trim_state` - but `var_flags_4E` itself is *not* restored yet.
- The actual restore (`ld a, var_flags_4E_copy2; st a, var_flags_4E`)
  happens much later, around address `E37F` (inside chunk `E363`).
- A second, short-lived instance of the identical pattern appears right
  after that restore: `var_flags_4E` is re-aliased to `var_trim_state` just
  to call `update_lambda_stft`, then immediately committed back and left alone.

**Practical effect:** every `var_flags_4E` bit reference between `D931` and
the `E37F`-ish restore point means `var_trim_state`, not flags_4E's usual
"operating mode flags" (closed loop / RPM low / ECT warm / etc). This
affects chunks `D931`, `DC77`, `DD38`, `DD59`, `E112`, and the start of
`E363`. `var_flags_46` (a different variable) is **not** aliased and keeps
its normal meaning throughout.

**A readability alias was added** for this: `var_trim_state_alias` (`.equ
var_flags_4E`, same address, zero bytes changed) declared next to
`var_flags_4E`'s own declaration. It's been applied to every reference this
session actually traced and confirmed: `calc_inj_pw_base`'s own body, `reset_pw_ramp_limiter`,
`ramp_limit_inj_pw`, `ramp_limit_inj_pw_simple`, and the full body of `update_lambda_stft` (through
`locret_DB74`, including `sub_DB75`/`sub_DB77` - a short-lived instance of
the same trick called from a separate point later in the main loop, not
`D931`'s direct continuation). **Not yet applied:** `loc_DC77`'s body past
its entry commit, and chunks `DD38`/`DD59`/`E112`/start-of-`E363` - these
are provably still the same alias (no restore happens in between) but
haven't been read/traced, so the rename wasn't applied there to avoid
renaming code whose behavior isn't actually understood yet. Apply the same
rename once that code gets its own pass.

This pattern isn't unique to this one instance - watch for the same
save-current/load-other/operate/save-back/restore-original shape anywhere
`var_flags_4E` appears to be doing something inconsistent with its
documented bits. Confirmed: it recurs later in this same span with a
*different* variable, `unk_1CF` - see "Other aliasing instances found"
below.

---

## Structure (chunk `D931`)

### 1) Entry gating (`D931`-`D956`)

Using the trim_state alias: resets `var_cnt_6A` unless trim_state bits 6/5
and `var_lambda_avg` (0x76-0x8A window) indicate a specific stable-trim
condition. Consumer found (in `update_lambda_stft`'s body, called from the second,
short-lived alias instance much later in the main loop): `loc_DB34` checks
`cmp #03h, var_cnt_6A` before deciding whether to set trim_state.5, i.e.
`var_cnt_6A` gates a "this stable-trim condition has held for at least 3
ticks" check.

### 2) Open-loop vs closed-loop path selection (`D956`-`D989`)

Gated on:
- `var_cnt_EA` (warm-up elapsed)
- `var_io_input1.5` (diagnostic check mode - skip if active)
- ECT, against a trim_state-dependent threshold (0xE1 or 0xE3)
- CPU2 enrichment request flags (`dmarx_fuel_trim_231`,
  `dmarx_warmup_enrich`, `dmarx_idle_enrich` - any nonzero forces the
  fallback/open-loop path)
- `var_flags_40.7`
- `var_flags_46.1` (the *real* closed-loop flag, not aliased)
- `dmarx_iscv_duty == 0x40` (nominal idle duty - if already at nominal,
  skip the closed-loop-specific init)

Calls `init_pw_closed_loop` (closed-loop init: `var_pw_loop_mode = 0xC8`) or `init_pw_open_loop`
(open-loop init: `var_pw_loop_mode = 0`), both of which fall into shared code
resetting `unk_1C0` and `unk_1C6` (`= 0xCCCD`, see the ramp-limiter
constant below).

### 3) VE-map candidate calculation (`D998`-`DA10`)

Multiplies CPU2's DMA'd VE-map words - `dmarx_word_226`, `dmarx_word_228`,
`dmarx_word_22A` - by a fixed constant (`0x1EB8`) via `mult_rDrX` (16x16→32
unsigned multiply, `D = D*X/256`, `X` = the product's high word/overflow
indicator).

**Corrected understanding** (the original pass here misread `mov`'s
direction - see the note above): at several points the code deliberately
substitutes a multiply's high word (from `X`, via `mov x, d` meaning
`D = X`) in place of the normal `/256`-scaled result, rather than simply
saving a copy of the scaled result as originally described. Specifically:
after the second `mult_rDrX` (by `dmarx_word_22A`), the high word is
compared against `dmarx_word_228`; if it exceeds it, the excess is scaled
by a `0xC8/256` (~78%) factor via `mult_rArX` and divided into
`dmarx_word_226`, with the quotient checked against `200`. Both the
"exceeds" and "doesn't exceed" paths converge on a second `mult_rDrX` (by
`dmarx_word_22A` again) whose high word is divided by either
`dmarx_word_226+dmarx_word_228` or `dmarx_word_228` alone, producing the
final candidate stored in `unk_1C0`. The `0xC8`/`200` value doubles as a
tag carried in `var_pw_loop_mode` for the two paths.

**Why the high-word substitution (resolved):** `mult_rDrX` doesn't just
return an overflow indicator in `X` - it **auto-saturates its own `D`
output to `0xFFFF`** whenever the high word is nonzero (confirmed by
reading its body: it checks the MSW and clips `D` before returning). The
`mov x, d` substitutions here are the caller deliberately **overriding that
lossy clip**: instead of accepting a flat `0xFFFF` (all magnitude
information lost), the code recovers the true proportional value from the
high word - a coarser but still-meaningful scale - whenever the fine-scale
(`/256`) result would otherwise have pinned to max. This is a deliberate,
sensible fixed-point technique (falling back to a wider/coarser scale to
preserve dynamic range), not an oddity - reasonably confident in this now.

**Resolved - which DMA word is which:** cross-referenced against CPU2's ROM
(`D151803-9661`, working copy now at `3S-GTE/D151803-9661/Claude/`). CPU1
and CPU2 share the same physical DMA buffer at a fixed address offset
(CPU1_addr = CPU2_addr + `0xDA`, confirmed via three independent
already-cross-named variable pairs, e.g. CPU1's `dmarx_max_retard_23B_161`
= CPU2's `dmatx_max_retard_161`). The three words matched by structural
position (three consecutive word-sized DMA slots on both sides - the exact
offset has a 1-byte discrepancy specifically in this region, likely a
padding/alignment byte elsewhere in the buffer, so position-matching was
used instead of the numeric formula):

| CPU1 name | CPU2 name | CPU2 computation |
|---|---|---|
| `dmarx_word_226` | `dmatx_ve_corr_map` (was `dmatx_map_table_unk_14D`, renamed in a later CPU2 session) | `table_ve_corr_map` lookup indexed by `dmarx_pim2` (MAP, received from CPU1), `/32` - a MAP-only VE/fuel correction table. Computed inside `calc_ignition_timing`, not `calc_params` - see that ASM's own header comments |
| `dmarx_word_228` | `dmatx_ve_corr_map_tps` (was `dmatx_unk_14F`) | `map_ve_corr_map_tps` bilinear lookup indexed by `dmarx_pim2` (MAP) and `dmarx_tps` (TPS, from CPU1), `/32` - forced to 0 when `dmarx_var_flags_46.2` is set (CPU1's idle-debounce flag, relayed back to CPU2 via DMA). Also computed inside `calc_ignition_timing` |
| `dmarx_word_22A` | `dmatx_ve_x_pim_x_rpm` (was `dmatx_unk_151`, = `var_ve_x_pim_x_rpm`, saturated) | `var_map_ve` (CPU2's base VE map, `map_c006_ve`, indexed by RPM and MAP) multiplied by `var_rpm_x_5p12` and by `dmarx_pim2/16`, i.e. **VE × MAP × RPM** - the classic speed-density airflow/load term |

**Also resolved (separate from the three DMA words above):** CPU1's
`dmarx_scaled_ve` (`0x022C`) = CPU2's `dmatx_scaled_ve` (`0x0153`, verified
via the `0xDA` offset formula) - `mult_rDrX_saturate(var_map_ve + 0x51,
0x200F)`, a differently-rescaled copy of the base VE map. This was
previously mis-cross-referenced in `calc_params`'s own header comment as
"= CPU1's `dmarx_word_226`" (a genuine inconsistency, since two CPU2
outputs can't both equal one CPU1 variable) - corrected there. Consumed
on CPU1's side in chunk `CE6C`'s accel/idle-enrichment scaling
(`divide_d_by_x:loc_E4EB`, as a `mult_rDrX` operand), not by
`calc_inj_pw_base` at all - i.e. it's unrelated to the speed-density base
fuel load calculation this section otherwise covers.

**Also resolved:** `calc_params`'s five single-byte ignition-timing/ISCV
DMA outputs (the group its own header previously marked "not examined,
out of scope") all match CPU1 counterparts exactly via the `0xDA` offset
formula (no padding discrepancy for these, unlike the three words above):

| CPU1 name | CPU2 name | CPU2 computation |
|---|---|---|
| `dmarx_ign_timing_fallback1` | `dmatx_ign_timing_fallback1` | `table_ign_rpm1(RPM)` - substituted for `dmatx_ign_timing` by CPU1's `sub_E865` when `var_diag_errors_5.0` is set at that point in its own computation - **not** a knock-sensor fault despite the flag's name; see the correction below |
| `dmarx_ign_timing_fallback2` | `dmatx_ign_timing_fallback2` (was `dmatx_ign_timing_unk_165`) | `table_ign_rpm2(RPM)` - same fallback role, substituted for `dmatx_ign_timing_unk_166` |
| `dmarx_iscv_duty` (CPU1 originally named it `dmarx_unk_242_168`, embedding this address) | `dmatx_iscv_duty` (was `dmatx_unk_168`) | `table_C376_rpm(RPM)/32` - base/nominal ISCV duty, refined by CPU1's own idle control loop (see `idle_control_system.md`) |
| `dmarx_ign_timing_unk_166` | `dmatx_ign_timing_unk_166` | `(table_C356_rpm(RPM) * table_C36A_ect(ECT))/64`, saturated |
| `dmarx_unk_241_167` | `dmatx_unk_167` | `(table_C360_rpm(RPM) * table_C370_ect(ECT))/64`, saturated |

`sub_E865`, CPU1's consumer of all five (plus the primary `dmarx_ign_timing`
and `dmarx_ign_timing_unk_166`), has since been partially traced (see
session_journal.md): a PIM-table-baseline-vs-clamp update involving
`unk_129`/`12B`/`12D`/`12F` state, self-re-armed to run once per ~32ms via
`var_schedule_flag_41.3`'s `tbs`-based one-shot gate (see that variable's
ASM declaration comment for the mechanism).
**Correction:** the fallback-vs-primary selection is **not** knock-sensor
fault handling despite `var_diag_errors_5.0`'s name and the function's
proximity to real knock code - `set_knock_sensor_err_flag`/
`check_knock_sensor_err_flag` are a repo-wide reused negate/abs() idiom
(see their own header comment and session_journal.md's architecture notes),
and `sub_E865` uses that bit purely as its own local "did the clamped
PIM-baseline value drop since last tick" signal. The middle blend's exact
arithmetic (the `mul`/`mult_rDrX`/`mov` sequence around `table_C163`) wasn't
fully re-derived with confidence and is still open CPU1 work.

So this calculation is confirmed to be a **speed-density base fuel
load computation**: CPU2 looks up VE from a MAP/RPM table, forms the
VE×MAP×RPM product as the primary load term (`dmarx_word_22A`), and
supplies two MAP-based correction/reference tables alongside it
(`dmarx_word_226`, `dmarx_word_228`) that CPU1's `calc_inj_pw_base` uses as
the comparison/reference and correction terms in section 3 above. The
specific thresholds
(`200`, the `0xC8/256` ratio) and exactly what physical quantity the final
`unk_1C0` candidate represents are also not confirmed.

### 4) Rate-limited blend (`DA10`-`DA60`)

Every ~488ms (`var_4ms_cnt_B5 >= 0x7A`): nudges `var_inj_pw_base` by
`+0x0C`/`-0x02` based on `var_lambda_integrator` vs `var_adc_lambda`'s
sign, clamped via `ram_1BE_limits` (range `0x0000`-`0x0500`, i.e.
0-~5.12ms of injector time - matches known PW constants elsewhere, e.g.
`injector_cold_start`'s `0x04E2`/`0x09C4`). In closed-loop mode
(`var_pw_loop_mode == 0xC8`) also overwrites `unk_1C0` with `var_adc_lambda` itself.
Then calls `ramp_limit_inj_pw_simple` when `var_adc_lambda` (**not**
`var_inj_pw_base` - see the correction under "The ramp-limiter cluster"
below) is below `0x4D`.

**Confirmed: the `var_lambda_integrator` threshold checks (`cmp #66h,
var_lambda_integrator` / `cmp #76h, var_lambda_integrator`) are high-byte-only
comparisons.** Checked the assembler source (`roms/d8x_assembler/instruction.py`):
`cmp #nn, nn` encodes as opcode `0x79` with two 8-bit operands (immediate,
direct-page address) - a genuine 8-bit compare against a single byte, not
the full 16-bit variable. Since `var_lambda_integrator` is `0x8000` at
stoich, thresholds `0x66`/`0x76` (as a high byte) correspond to roughly
`0x6600`-`0x76FF` - both below stoich, consistent with the lean-side
interpretation used above.

### 5) Hand-off to `loc_DC77`

Commits the trim_state alias back to `var_trim_state` but does not yet
restore the real `var_flags_4E`. `loc_DC77` and its continuation
(`DD38`/`DD59`) turned out to be a **different kind of code entirely** -
not fuel-pulse-width calculation, but a periodic I/O-debounce and
diagnostic/error-flag-checking phase of the main loop. Full detail in the
header comment above `loc_DC77` in the ASM; summary:

- Two RPM-hysteresis blocks updating `var_flags_4F.0` and resetting the
  `var_4ms_cnt_B6`/`var_4ms_cnt_B7` debounce timers (shared with `calc_iscv`
  and chunk `D3A5`).
- **A third, independent instance of the aliasing trick**, this time
  aliasing `var_flags_4E` to `unk_1CF` (not `var_trim_state`) - see
  "Other aliasing instances found" below.
- `loc_DD59` jumps to `loc_E112`, not yet traced.
- `loc_DD69` (reached via `jsr` from a *different*, later point in the main
  loop - the short second `var_trim_state`-alias instance that also calls
  `update_lambda_stft`) wraps a long run of O2-heater/lambda/coolant-sensor
  diagnostic checks in its own `unk_1CF` alias instance, and resolves two
  previously-unexamined helpers: `sub_DE5A` (resets `unk_187`, sets
  `var_flags_4F.7` if outside `[3, 0x131)`) and `sub_DE71` (saturating
  increment of `unk_187`).

---

## Other aliasing instances found

Beyond `var_trim_state`, the `var_flags_4E`-as-scratch trick recurs with
**at least one other variable**: `unk_1CF`, in two confirmed short-lived
windows within the `DC77`/`DD38`/`DD69` diagnostic phase (a battery/starter
latch, and an O2-heater/lambda latch). A matching `unk_1CF_alias` `.equ`
was added next to `unk_1CF`'s declaration and applied to both windows.
`unk_1CF` itself wasn't renamed to something more meaningful - unlike
`var_trim_state`, it has no pre-existing identity to borrow, and might
simply be a generic local scratch bitfield rather than "impersonating" a
specific other variable.

**Resolved: `var_flags_4E_copy_2`** (note the underscore; genuinely
distinct from `var_flags_4E_copy2`, which the `var_trim_state` alias uses)
is **not** a protect-across-a-big-window mechanism, and does **not**
threaten the "real `var_flags_4E`" interpretations used in chunks
`C9DA`/`CB1E`/`D1DD`/`D3A5`, `calc_iscv`, or `calc_4ms_corrections`.

It's a **last-known-good real-value cache**: refreshed with the current
real `var_flags_4E` at multiple independent points (confirmed at chunk
`CB1E`'s `loc_CD0B`, right after the boost-limit-flag update, and again at
`calc_4ms_corrections`' `loc_EF48`), and consulted/restored at other points
(the continuation right after chunk `C9DA`, and `loc_DF2A` inside the
`DC77`/`DD69` diagnostic phase) whenever code needs a valid real value that
isn't otherwise fresh - typically because `var_flags_4E` is mid-excursion
for a different purpose (the `var_trim_state` or `unk_1CF` aliases). Each
write simply refreshes the cache for whichever read happens next in
execution order; it is not one dedicated write paired with one specific
later read, which is why its read/write sites don't line up in a simple
1:1 sequence when read in file/address order. Confirmed by reading the
code immediately following the `loc_DF2A` restore: it uses
`var_flags_4E.7` exactly as documented (boost-limit/overpressure error),
consistent with a genuine real value having been restored there.

---

## The ramp-limiter cluster (`reset_pw_ramp_limiter`/`init_pw_closed_loop`/`init_pw_open_loop`/`ramp_limit_inj_pw`/`ramp_limit_inj_pw_simple`)

All five functions share a `0xCCCD` constant, which in Q16 fixed point is
`0xCCCD / 0x10000 ≈ 0.8` - an ~80%-per-step exponential approach, i.e. a
rate limiter that lets the working pulse-width move only ~20% of the way
toward a new target each time it's applied, rather than jumping straight to
it. This is a very standard technique to avoid abrupt injector pulse-width
steps causing driveability issues.

- **`reset_pw_ramp_limiter`**: resets `unk_1C2`/`var_inj_pw_base`/`unk_1C4` all to
  `0xCCCD`, and clears `var_trim_state.5` (via the alias). Called from
  `loc_DA94`'s area (not deep-dived) - likely on a mode transition that
  should discard the ramp state entirely.
- **`init_pw_closed_loop`/`init_pw_open_loop`**: see above (path init).
- **`ramp_limit_inj_pw`**: the main blend step, called every ~488ms from
  `calc_inj_pw_base`'s continuation, and once more directly from
  `calc_inj_pw_base` itself in a "diagnostic only" mode (see `trim_state.4`
  below). **Fully traced this session** (branch-by-branch, with `mov`
  direction re-verified at every step) - see "Branch-by-branch trace"
  below for the complete walkthrough.
- **`ramp_limit_inj_pw_simple`**: a shorter, single-path variant of the same
  pattern: `D = var_inj_pw_base / (unk_1C4 - 0xCCCD)` via `divide_d_by_x`,
  then `+/-0xCCCD`-adjusted (sign depending on whether the divide's error
  flag fired) and stored to `unk_1C2`. Also sets/clears `var_trim_state.2`
  (via the alias) based on a `0xC7AE` threshold.

  **Correction to a prior-session claim:** this is called from `loc_DA58`
  when **`var_adc_lambda` (signed lambda sensor voltage) is below `0x4D`**,
  not `var_inj_pw_base` as previously stated. `X` is loaded from
  `var_adc_lambda` at `loc_DA17` and never reloaded before the `loc_DA58`
  gate - `var_inj_pw_base` is loaded into `D` in that same block for an
  unrelated small lambda-driven nudge (`+0x0C`/`-0x02`, clamped via
  `ram_1BE_limits`) that has already completed by the time the gate is
  checked.

### Branch-by-branch trace of `ramp_limit_inj_pw`

Entry: `X = unk_1C0` (the current candidate), `trim_state` bits 0/1/3/4 are
read/written throughout (via the alias - real `var_flags_4E` bits, not
`var_trim_state`'s own documented meaning during this span).

1. **`trim_state.4` set on entry** (checked again, unchanged, at `loc_DBDB`
   since nothing between clears it): "diagnostic-only" mode. Computes
   `D = unk_1C2 - 0xCCCD`, error-flags via `set_knock_sensor_err_flag` if
   negative (ratio below nominal), then `D = mult_rDrX(D, unk_1C0)`
   (deviation scaled by the candidate) `+/- 0xCCCD` (sign per whether the
   error flag fired), and exits via `loc_DC3A` **without** touching
   `var_inj_pw_base`/`unk_1C0`/`unk_1C6` - the computed `D` is left for the
   *caller* to consume. `calc_inj_pw_base`'s `loc_D9FA` is exactly this
   caller: it force-sets `var_inj_pw_base` itself, sets `trim_state.4`, calls
   `ramp_limit_inj_pw`, then stores the returned `D` into `unk_1C4`.
   `loc_DC3A` clears `trim_state.4` - the flag is "consumed" by being
   handled once.
2. **`trim_state.4` clear + `unk_1C0 == var_inj_pw_base`** (candidate
   unchanged since last call): fast path, `D = unk_1C4` (last
   carried-forward value), skips straight to the ceiling check
   (`loc_DBDE`) - no ratio/error-flag pass runs at all.
3. **`trim_state.4` clear + candidate changed**: runs the same
   ratio-deviation computation as case 1, then falls into the ceiling
   check with the freshly-computed `D`.
4. **`loc_DBDE`** compares `D` against `unk_1C8` (see below for what this
   is):
   - `D <= unk_1C8`: falls into `loc_DBF1` (blend-toward-ceiling).
   - `D > unk_1C8`, `trim_state` bits 0 **and** 1 both set, and `unk_1C4`
     (the prior carried-forward value) is **not** itself already above
     `unk_1C8`: also falls into `loc_DBF1`.
   - `D > unk_1C8` and (`trim_state.0` clear **or** `trim_state.1` clear):
     simplest exit (`loc_DC35`/`DC37`) - clears `trim_state.3`, stores `D`
     into `unk_1C6`, done. `var_inj_pw_base`/`unk_1C0`/`unk_1C4` untouched.
   - `D > unk_1C8` **and** `unk_1C4` also already `> unk_1C8` (sustained
     over-ceiling): `loc_DC24` - in closed-loop mode (`var_pw_loop_mode == 0xC8`)
     only, resets `unk_1C0` back to `var_inj_pw_base` (discards the stale
     candidate); either way clears `trim_state.3` and stores the original
     `loc_DBDE`-entry `D` (the candidate/blend value itself, **not** the
     ceiling) into `unk_1C6`.
5. **`loc_DBF1`** (blend-toward-ceiling): sets `trim_state.3`, computes
   `X = 0xCCCD - unk_1C8`, `D = 0xCCCD - unk_1C2`.
   - `unk_1C2 >= 0xCCCD` (at/above nominal ratio): `D = var_inj_pw_base`
     unchanged, skip the divide.
   - `unk_1C2 < 0xCCCD` (below nominal): `D = (0xCCCD-unk_1C2) /
     (0xCCCD-unk_1C8)` via `divide_d_by_x`, clamped to `[0,0x0500]` via
     `ram_1BE_limits`, and stored into **`unk_1C0`** - the candidate itself
     gets refined here, not just `var_inj_pw_base`.

     Either way: if `trim_state.0` is clear, commits `D` to
     `var_inj_pw_base` and stashes the ceiling (`unk_1C8`, popped back off
     the stack) into `unk_1C4`; if `trim_state.0` is set, both are left
     alone. `unk_1C6` always ends up holding the ceiling value (`unk_1C8`)
     on this path, regardless of `trim_state.0`.

**Key finding:** `unk_1C0`/`unk_1C4`/`unk_1C6` do **not** have single fixed
identities ("the candidate" / "the carried-forward value" / "the
ceiling"). Each gets overwritten with a different one of {fresh VE-map
candidate, `var_adc_lambda`, the `unk_1C8` ceiling, the ratio-deviation
result, `var_inj_pw_base`} depending on which branch runs - see the
Variable Reference below for the full per-branch inventory. This is why a
clean per-variable rename was never found even after this full trace:
there isn't one to find. `unk_1C2` is the one exception with a stable
role - it's `ramp_limit_inj_pw_simple`'s output and `ramp_limit_inj_pw`'s
deviation input, always a ratio nominally around `0xCCCD`.

**`unk_1C8`'s producer - RESOLVED:** its immediate write site is `loc_E6A8`
(`st d, unk_1C8`), fed by a `dmatx_pim`/`var_pim2`-linked computation, which
confirms the "MAP/PIM-pressure-linked" characterization used above. That
computation sits at the tail of **`calc_dmatx_pim`** (was `sub_E551`,
`~E551`-`E6B0`+, 350+ bytes).

An earlier pass guessed that function was a "knock/PIM-linked limiting
calculation, possibly boost/overpressure-related". **That was wrong.** It is
manifold-pressure transient compensation: its single exit stores to
`dmatx_pim`, the value CPU2 fuels from. The `set_knock_sensor_err_flag` /
`check_knock_sensor_err_flag` calls that suggested knock involvement are only
the generic abs()/restore-sign primitive those functions actually implement -
they carry no knock meaning here, and the old `var_unk_knk_*` names on its
state came from the same mistake.

Briefly: a MAP sensor lags the real manifold event, so the ECU also builds a
throttle-derived pressure estimate (`var_pim_tps_est`), filters it twice at
different rates (`var_pim_est_fast`, `var_pim_est_slow`), and uses the
divergence between those filters as a "load is changing" measure that
corrects `var_pim2` on its way to `dmatx_pim`. `unk_1C8` is simply where one
intermediate of that computation lands for `ramp_limit_inj_pw`'s purposes.

See `calc_dmatx_pim`'s own header in the disassembly for the full chain, and
`session_journal.md` for the renames this produced.

**Consequence for fuel trim:** the same function publishes
`var_pim_trans_fast`, and `closed_loop_control` refuses to run trim learning
while that indicates a transient - the standard "freeze fuel trim through a
transient" rule. Any future work on this ECU's short-term vs long-term trim
split should start from there.

---

## Short-term vs long-term fuel trim

Both exist, and both are applied at one place: `sub_E454`'s `loc_E47B`
assembles a single multiplier and applies it to the injector pulse width.

```
multiplier = 0x0100 (unity)
           + read_nv_afr_trim(load)   <- LONG-term  (LTFT)
           + var_lambda_integrator    <- SHORT-term (STFT)
```

**STFT - `var_lambda_integrator`.** The fast O2 feedback integrator. Plain
RAM, never written to NV, forced back to its `0x8000` neutral on reset
events (`loc_D04F`). This is what swings cycle to cycle as the sensor
crosses rich/lean.

Its controller is **`update_lambda_stft`** (was `loc_DA63`), a textbook
**jump-and-ramp** (proportional + integral) O2 control law:

- **Jump** - proportional, large, on a rich/lean transition (`loc_DABF`):
  `var_lambda_avg >= 0xB3` (rich) adds `0x07AE` to `unk_1C4`; `<= 0x4D`
  (lean) subtracts it. `0x4E`-`0xB2` is a **deadband** that exits without
  acting - that is what keeps the loop from chattering around stoich.
- **Ramp** - integral, small, every tick while held on one side
  (`loc_DB41`): +/-`0x0010` following `var_adc_lambda`'s sign, each gated on
  the integrator not already sitting at its `0x85` / `0x76` rail.

Alongside the jump it steps the integrator itself by `0x0F5C` (clamped to
`0x1A00` rich / `0xE600` lean) and `var_lambda_avg` by `0x0F` (clamped `0x1A`
/ `0xE6`). Those clamp pairs are the same numbers scaled by 256, so
`var_lambda_avg` deliberately tracks the integrator's high byte - do not
treat the two as independent quantities.

Entry requires RPM above `0x14`, PIM above `0x16`, and `var_flags_46.2`
(throttle closed long enough). When any fails, `var_stft_dwell_cnt` is
cleared; the controller may only change closed-loop mode once that
free-running counter has reached `0x40` with conditions holding - a hold-off
against brief disturbances.

**LTFT - the `nv_afr_trim_base` table.** Twelve bytes in battery-backed NV
RAM, so it survives power cycles. `read_nv_afr_trim` indexes it by
`var_pim2` (manifold pressure, i.e. load) and interpolates between cells,
with two special cases: it returns the first/idle cell directly whenever
`var_flags_46.2` says the throttle is closed, and a neutral `0x80` when
`var_flags_42.0` says the trims are not valid. Validated at startup against
`nv_afr_trim_top`/`nv_afr_trim_end`; a failure wipes all of NV RAM through
`clear_nv_ram`.

### A third, separate learned correction

`var_nv_trim_unk_96` is **not** part of the multiplier above, despite being
learned from the same O2 sensor. `closed_loop_control` accumulates polarity
votes into `var_o2_vote_accum` (+/-1 per sample from a `0x80` neutral) over a
17-sample window counted by `var_o2_vote_cnt`, discarding the first seven
samples as settling, then applies a majority verdict with a `0x7D`-`0x83`
deadband to move the stored value by exactly one step.

Its gating is much tighter than the LTFT table's - ECT 82.9-103.8 C,
off-idle, RPM < 3200, battery >= 11.4 V, no transient
(`var_pim_trans_fast`), no CPU2 fuel-trim or idle-enrich request, and
`var_trim_state == 4` - which reads as a slow, cruise-only global
correction. It is stored as a value/complement pair (the halves stepped in
opposite directions), and `sub_D2C5` wipes all NV RAM if it fails validation
against `nv_96_limits`.

**Where it is spent - resolved.** Its consumers are `sub_E843` on CPU1 and
CPU2 via `copy_dma_tx`'s `dmatx_nv_trim_o2`. CPU2 receives it as
`dmarx_nv_trim_o2` and, in `check_startup`'s `loc_CB6D` chunk, multiplies an
ECT-indexed `table_C393` lookup by it; the product becomes
`var_enrichment_unk_100`, the warm-up enrichment term that
`decay_enrichment_unk_100` subsequently bleeds away. Gated there on
`dmarx_ect` between `0x0F` and `0xC4`.

So it is a **fuel** correction, not a timing one: CPU1 learns it slowly from
the O2 sensor under cruise, and mostly spends it through CPU2's enrichment
path - which is why it never appears in CPU1's own STFT+LTFT multiplier
above.

> **DMA offset, for anyone following this across the two ROMs.** The
> `CPU1 = CPU2 + 0xDA` relation applies to the **CPU2 -> CPU1** direction.
> The **CPU1 -> CPU2** (`dmatx`) direction uses **`CPU1 = CPU2 + 0x13B`**.
> Applying 0xDA to a `dmatx` address lands inside CPU2's `var_serbus_rx`
> buffer. The 0x13B figure is confirmed by the name pairs that already
> matched independently across the buffer (`dmatx_pim2`/`dmarx_pim2`,
> `dmatx_tps`/`dmarx_tps`, `dmatx_ect`/`dmarx_ect`,
> `dmatx_lambda_state`/`dmarx_lambda_state`).

### Trim learning is frozen through transients

`closed_loop_control` refuses to learn while `var_pim_trans_fast` indicates
load is changing (`cmp a,#0FEh` / `blta`). That value comes from
`calc_dmatx_pim`'s lead/lag filter pair - see the manifold-pressure section
above. It is the standard "freeze fuel trim through a transient" rule: trims
only adapt once the fast and slow pressure estimates reconverge.

---

## Variable Reference

| Variable | Description |
|---|---|
| `var_inj_pw_base` | Working base injector pulse-width (was `unk_1BE`), clamped to 0x0000-0x0500 via `ram_1BE_limits` |
| `var_pw_loop_mode` | Open-loop (0) vs closed-loop (0xC8) path selector, set by `init_pw_open_loop`/`init_pw_closed_loop` |
| `unk_1C0` | The VE-map candidate, but reused as scratch: also overwritten with `var_adc_lambda` (DA10-DA60, closed-loop), the `unk_1C8` ceiling-driven divide result (`ramp_limit_inj_pw`'s `loc_DBF1`), or `var_inj_pw_base` (`loc_DC24`, closed-loop). No single fixed identity - see `ramp_limit_inj_pw`'s branch trace |
| `unk_1C2` | Ratio value nominally `0xCCCD` (~0.8 in Q16) - `ramp_limit_inj_pw_simple`'s output, `ramp_limit_inj_pw`'s deviation input. The one variable in this cluster with a stable role |
| `unk_1C4` | Carried-forward PW-scale value at `ramp_limit_inj_pw`'s entry, but overwritten with the `unk_1C8` ceiling in `loc_DBF1` when `trim_state.0` clear, or with the ratio-deviation result by `calc_inj_pw_base`'s diagnostic-only call. No single fixed identity |
| `unk_1C6` | The final per-call output register of `ramp_limit_inj_pw` - ends up holding the ceiling (`unk_1C8`) on the `loc_DBF1` path, or the candidate/blend value on the `loc_DC24`/`loc_DC35` paths |
| `unk_1C8` | A PIM/MAP-pressure-linked bound compared against PW-scale values in `ramp_limit_inj_pw`. Producer partially traced to `loc_E665` (~`E620`-`E6B0`), which folds `var_pim2`-derived `dmatx_pim` into it - the rest of that computation isn't traced (see Open Questions) |
| `var_cnt_6A` | Reset by `calc_inj_pw_base`'s entry gate; consumer not traced this session |
| `dmarx_word_226` | CPU2's MAP-only VE/fuel correction table (`table_map_unk_C53D`, indexed by MAP) |
| `dmarx_word_228` | CPU2's MAP+TPS bilinear VE correction (`map_map_tps_C51F`), zeroed during idle debounce |
| `dmarx_word_22A` | CPU2's VE×MAP×RPM speed-density load term (`var_ve_x_pim_x_rpm_unk_10C`, saturated) |
| `var_trim_state` | Persistent trim-state value; aliased into `var_flags_4E` for a large span of the main loop (see above) |

---

## Open Questions (not resolved this session)

- `unk_1C8`'s full producer chain: traced as far as `loc_E665`
  (~`E620`-`E6B0`) and confirmed it folds in `var_pim2`-derived
  `dmatx_pim`, but the surrounding computation (`var_pim_tps_est`,
  `var_pim_est_fast`/`135`, `var_nv_trim_unk_98`, `unk_1CA`,
  `divide_rD_64_saturate`/`divide_rD_16`/`mult_rArX`) isn't traced. This
  sits inside the still-largely-unexplored `E363`-onward region flagged
  below - worth resolving together with that pending work rather than as
  an isolated one-off.
- `loc_DC77`'s body past its entry commit, and chunks `DD38`/`DD59`/`E112`/
  start-of-`E363` - all still under the `var_flags_4E`-is-`var_trim_state`
  alias (confirmed), not yet read/traced or renamed.
- `update_lambda_stft`'s body (traced and renamed this session, called from a short
  second alias instance later in the main loop) touches
  `var_lambda_avg`/`var_lambda_integrator` in ways that look like another,
  separate lambda-trim adjustment mechanism (distinct from both the
  zone-based `nv_afr_trim_base` system and chunk D1DD's
  `var_nv_trim_unk_96` system) - not fully characterized, just renamed for
  the alias.

---

*Derived from IDA disassembly of D151803-9651 (CPU1), Toyota 3S-GTE ECU.*
