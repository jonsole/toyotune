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
  to call `loc_DA63`, then immediately committed back and left alone.

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
`ramp_limit_inj_pw`, `ramp_limit_inj_pw_simple`, and the full body of `loc_DA63` (through
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
condition. Consumer found (in `loc_DA63`'s body, called from the second,
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
- `unk_40.7`
- `var_flags_46.1` (the *real* closed-loop flag, not aliased)
- `dmarx_iscv_duty == 0x40` (nominal idle duty - if already at nominal,
  skip the closed-loop-specific init)

Calls `init_pw_closed_loop` (closed-loop init: `unk_1BD = 0xC8`) or `init_pw_open_loop`
(open-loop init: `unk_1BD = 0`), both of which fall into shared code
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
tag carried in `unk_1BD` for the two paths.

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
| `dmarx_word_226` | `dmatx_map_table_unk_14D` | `table_map_unk_C53D` lookup indexed by `dmarx_pim2` (MAP, received from CPU1), `/32` - a MAP-only VE/fuel correction table |
| `dmarx_word_228` | `dmatx_unk_14F` | `map_map_tps_C51F` bilinear lookup indexed by `dmarx_pim2` (MAP) and `dmarx_tps` (TPS, from CPU1), `/32` - forced to 0 when `dmarx_var_flags_46.2` is set (CPU1's idle-debounce flag, relayed back to CPU2 via DMA) |
| `dmarx_word_22A` | `dmatx_unk_151` (= `var_ve_x_pim_x_rpm_unk_10C`, saturated) | `var_map_ve` (CPU2's base VE map, `map_c006_ve`, indexed by RPM and MAP) multiplied by `var_rpm_x_5p12` and by `dmarx_pim2/16`, i.e. **VE × MAP × RPM** - the classic speed-density airflow/load term |

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

Every ~488ms (`var_4ms_cnt_B5 >= 0x7A`): refines the candidate against
`var_lambda_integrator`'s direction and `var_adc_lambda`'s sign, clamps
`var_inj_pw_base` via `ram_1BE_limits` (range `0x0000`-`0x0500`, i.e.
0-~5.12ms of injector time - matches known PW constants elsewhere, e.g.
`injector_cold_start`'s `0x04E2`/`0x09C4`), and conditionally calls
`ramp_limit_inj_pw_simple`.

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
  `loc_DA63`) wraps a long run of O2-heater/lambda/coolant-sensor
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
- **`ramp_limit_inj_pw`**: the main blend step, called every ~488ms. Compares
  `unk_1C0` (candidate) against `var_inj_pw_base` (current) and `unk_1C8`
  (a third reference, not traced), applies the 0.8x ratio via
  `mult_rDrX`/`divide_d_by_x`, and on out-of-range results calls
  `set_knock_sensor_err_flag` (`var_diag_errors_5.0` - a general
  "computation out of range" signal reused across several subsystems, not
  literally knock-specific; the same bit is touched by unrelated threshold
  checks in `calc_iscv`). The branch-by-branch logic
  (`loc_DBB5`/`DBDE`/`DBF1`/`DC0C`/`DC17`/`DC24`/`DC34`/`DC35`/`DC37`)
  wasn't fully traced - the overall shape (rate-limited exponential
  approach with overflow flagging) is clear, the exact branch conditions
  are not.
- **`ramp_limit_inj_pw_simple`**: a shorter, single-path variant of the same pattern,
  operating on `unk_1C4`/`var_inj_pw_base` directly. Called from `loc_DA58`
  when `var_inj_pw_base < 0x4D` (a very small pulse width) - likely a
  dedicated path for the near-zero/idle-fuel edge case where `ramp_limit_inj_pw`'s
  general blend would be numerically awkward. Also sets/clears
  `var_trim_state.2` (via the alias) based on a `0xC7AE` threshold.

---

## Variable Reference

| Variable | Description |
|---|---|
| `var_inj_pw_base` | Working base injector pulse-width (was `unk_1BE`), clamped to 0x0000-0x0500 via `ram_1BE_limits` |
| `unk_1BD` | Open-loop (0) vs closed-loop (0xC8) path selector, set by `init_pw_open_loop`/`init_pw_closed_loop` |
| `unk_1C0` | A pulse-width candidate distinct from `var_inj_pw_base` - not renamed, precise distinction from `unk_1C4`/`unk_1C8` unclear |
| `unk_1C2` | Ramp-limiter working value, reset to `0xCCCD` by `reset_pw_ramp_limiter`/`init_pw_closed_loop`/`init_pw_open_loop` |
| `unk_1C4` | Another pulse-width-scale value, used by `ramp_limit_inj_pw`/`ramp_limit_inj_pw_simple` |
| `unk_1C6` | Reset to `0xCCCD` alongside `unk_1C2`, role otherwise not traced |
| `unk_1C8` | A third reference value compared against in `ramp_limit_inj_pw`, not traced |
| `var_cnt_6A` | Reset by `calc_inj_pw_base`'s entry gate; consumer not traced this session |
| `dmarx_word_226` | CPU2's MAP-only VE/fuel correction table (`table_map_unk_C53D`, indexed by MAP) |
| `dmarx_word_228` | CPU2's MAP+TPS bilinear VE correction (`map_map_tps_C51F`), zeroed during idle debounce |
| `dmarx_word_22A` | CPU2's VE×MAP×RPM speed-density load term (`var_ve_x_pim_x_rpm_unk_10C`, saturated) |
| `var_trim_state` | Persistent trim-state value; aliased into `var_flags_4E` for a large span of the main loop (see above) |

---

## Open Questions (not resolved this session)

- The precise distinction between `unk_1C0`/`unk_1C4`/`unk_1C8` as "the"
  candidate at any given point in `ramp_limit_inj_pw`.
- `ramp_limit_inj_pw`'s exact branch-by-branch conditions.
- `loc_DC77`'s body past its entry commit, and chunks `DD38`/`DD59`/`E112`/
  start-of-`E363` - all still under the `var_flags_4E`-is-`var_trim_state`
  alias (confirmed), not yet read/traced or renamed.
- `loc_DA63`'s body (traced and renamed this session, called from a short
  second alias instance later in the main loop) touches
  `var_lambda_avg`/`var_lambda_integrator` in ways that look like another,
  separate lambda-trim adjustment mechanism (distinct from both the
  zone-based `nv_afr_trim_base` system and chunk D1DD's
  `var_nv_trim_unk_96` system) - not fully characterized, just renamed for
  the alias.

---

*Derived from IDA disassembly of D151803-9651 (CPU1), Toyota 3S-GTE ECU.*
