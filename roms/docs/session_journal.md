# D151803-9651 Reverse Engineering Session Journal

## Session overview
Reverse engineering of Toyota 3S-GTE ECU CPU1 ROM (Toshiba D8X / Denso 8X MCU).
Working file: D151803-9651.ASM (IDA Pro disassembly, latin-1 encoding, \r\n line endings)

---

## Completed subsystems

### CPU2 (D151803-9661): boot sequence and main control-flow backbone documented
First slice of the "broad prose-documentation gap" work: full write-up of
`reset_vector` -> `clear_variables` -> `loc_C88E` -> `main_loop` ->
`calc_rpm`, plus the runtime reset-recovery path that closes the loop.
Previously only individual pieces of the periodic-tick chain *past*
`main_continue` were documented - this fills in everything before it.

- **`reset_vector`**: raw hardware init. Notably, `ASR3` (the DMA TX
  register) is set to `dmatx_ve_corr_map`'s address - confirming that
  variable is literally the first byte of the entire inter-MCU DMA TX
  buffer, not just "one item in the fuel VE section" as documented
  elsewhere.
- **`clear_variables`**: zero-fills two RAM ranges (byte-wise
  `var_flags_40`-`unk_7F`, word-wise `var_ne_count`-`word_16D`) - the
  working-variable region, distinct from the reset-recovery path's clear.
- **`loc_C88E`**: a shared soft-init entry point reached both from cold
  reset and from the runtime reset-recovery path below - re-inits NE
  counters and several fixed-default flags/counters, primes ADC, re-arms
  interrupts, then spins until `unk_47.5` sets before falling into
  `main_loop`.
- **`main_loop`**: the actual free-running top-level loop - re-primes
  peripherals, conditionally processes a received DMA frame, runs several
  small per-tick debounce checks, computes RPM (via `calc_rpm`, or a
  stall/restart reset path when `var_cnt8ms_AF` is still low), then hands
  off to `main_continue` - the already-documented periodic-tick chain.
- **`calc_rpm`**: converts `var_ne_sum` to RPM via a normalize/table-
  lookup/de-normalize reciprocal approximation (left-shift to normalize,
  correction-table lookup, then the appropriate power-of-2 divide from
  the shared `divide_rD_128`-`divide_rD_2` cascade) - avoids a full
  division despite RPM's huge dynamic range. Exact table constants not
  independently re-derived; the overall algorithm shape is confirmed.
- **Closed the loop**: confirmed `loc_D2E9` (reached via the shared
  `IRQLL.0`/`PORTB.6` reset-detection check embedded in `check_startup`
  and `selfcheck_io_pump`, called throughout the periodic tick) does a
  hardware settle delay, clears RAM `0x0040`-`0x02FF` (a third,
  still-different range from `clear_variables`'s two), resets the stack,
  re-inits PORTA/PORTB/PORTD_ASRIN/DOUT, and jumps back to `loc_C88E` -
  i.e. a full soft-reset triggerable by any reset/interrupt condition
  detected anywhere in the tick chain, not just at power-on. Resolved a
  pre-existing tentative comment ("Some kind of reset...") into a
  confirmed explanation.

Full detail in the header comment above `reset_vector` in the ASM.

### CPU2 (D151803-9661): three flagged loose ends resolved
- **dmarx_word_226 double-claim, resolved**: verified via the DMA offset
  formula against both ROMs' assembled `.lst` files. `dmatx_ve_corr_map`
  (`0x014D`) is the real match for CPU1's `dmarx_word_226` (`0x0226`, off
  by the already-documented 1-byte padding). `dmatx_scaled_ve` (`0x0153`)
  does not match at all (7 bytes off) - it's actually CPU1's
  already-separately-named `dmarx_scaled_ve` (`0x022C`), just never
  cross-referenced. Fixed `calc_params`'s header on CPU2, added a
  cross-reference comment at the CPU1 consumer site (chunk `CE6C`'s
  `loc_E4EB`), and updated the DMA cross-reference table in
  `fuel_calculation_system.md` (which also had stale pre-rename CPU2
  variable names - fixed those too).
- **unk_51's consumer, resolved**: found `serial_debug_check` (already
  named, never examined) implements a generic "read arbitrary 16-bit RAM
  word by index" debug protocol over the K-line serial link, polled every
  4ms from `iv6_4ms_process`. Special index `0x1F` returns `rom_version`
  - an "identify device" query, confirming this is a live-data/debug-tool
  protocol with no apparent bounds-check on the index. `unk_51`'s "no
  in-ROM reader" finding from two sessions ago is now explained: its
  consumer is external tooling, not ECU logic - not dead code. Full
  protocol byte-framing not traced (a worthwhile future subsystem doc,
  flagged in Pending work).
- **main_continue's RPM-delta computation (loc_C9BC), resolved**: a gated
  low-pass filter for `unk_E8` (which feeds `update_rpm_filter_EA`).
  `dmarx_unk_D6`'s sign selects between two modes: negative (and RPM in a
  ~450-1250rpm idle-ish band) gives a slow ~1/32-per-call approach with a
  minimum-step-of-1 safeguard; otherwise `unk_E8` snaps directly to
  current RPM. `dmarx_unk_D6` itself (a signed byte from CPU1, used only
  by its sign here and at one other site) remains uncharacterized on
  CPU1's side.

### CPU2 (D151803-9661): loc_ sweep - audited for mis-classified routines
Swept all 284 `loc_` labels for the specific failure mode of "this is
actually a callable subroutine that never got promoted from IDA's default
name" - checked for every `jsr loc_*`/`bsr loc_*` call site (a `loc_`
label reached via a *call* instruction, not just a branch, is functionally
a subroutine regardless of its name). Found exactly **one**:
`loc_C613` (called 3x, all from the TVSV/warning-debounce area documented
last session) -> renamed to `table_pair_interpolate_rpm_entry`: an entry
point into the already-named `table_pair_interpolate` that defaults B=0
and preserves RPM in D across the call (table_pair_interpolate's own first
instruction stashes D for the duration).

Everything else among the 284 is a genuine internal branch target (`bra`/
`beq`/`bne`/etc.) within an already-named function - the expected, correct
state per IDA's convention (`loc_` = internal label, `sub_` = callable
routine), not an oversight. This sweep is a check, not a renaming task in
itself - no further "loc_ hiding a real function" work is needed on CPU2
unless new code gets traced that calls into a currently-unexamined `loc_`.

### CPU2 (D151803-9661): periodic warning/diagnostic-output debounce phase documented
`loc_D037`-`loc_D0B0` (immediately after the TVSV block's `drive_DOUT2_tvsv`,
scoped out as a separate item last session) - part of the same main-loop
periodic tick as the enrichment chain (falls through to `loc_D317` ->
`loc_D333` -> `update_ect_enrich_clamp`). Two independent debounced
warning checks:

1) **PORTA.2**, gated on `dmarx_battery >= 11.4V` and `dmarx_unk_D4 >
   0x0F`: compares an RPM x `dmarx_word_CB` product against a MAP-indexed
   2-point table (one of two curves, selected by a battery-voltage
   threshold - the higher-voltage curve is systematically lower/stricter),
   with a two-stage debounce (~2.9s then ~32ms) before setting the output.
   `dmarx_word_CB` is used nowhere else in the file - its physical meaning
   isn't confirmed. The shape (RPM/MAP/battery-dependent, decreasing
   threshold curve) is suggestive of a charging-system or load
   rationality check, flagged as a hypothesis, not a confirmed reading.
2) **PORTA.3**, gated on ECT/speed/RPM/PIM/THAM all simultaneously
   exceeding fixed thresholds (a combined high-load condition), debounced
   over ~4.9s. Suggestive of an overheat/high-load warning, likewise not
   independently confirmed.

Both use the same "clear a debounce counter unless condition holds, set
output once counter crosses a threshold" idiom seen elsewhere (e.g. the
TVSV limiter-cooldown scale). Full trace in the ASM header above `loc_D037`.

### CPU2 (D151803-9661): TVSV boost-control duty-cycle calculation documented
Full prose write-up for `loc_CE86`-`loc_D036` (previously flagged as an
"entirely unexplored" subsystem, but turned out to already have
meaningful variable names from earlier work - just never had the
connecting narrative). Computes `var_tvsv_117`, a 0-200 PWM duty consumed
by `drive_DOUT2_tvsv` to drive the TVSV solenoid via `DOUT.2` (confirmed:
`var_tvsv_cnt` free-runs 0->200, `DOUT.2` high while `var_tvsv_cnt <
var_tvsv_117`).

Calculation shape: a limiter-cooldown scale (`var_tvsv_scale_limiter`,
reduced for ~1s after a CPU1 boost/rev-limiter event) combines with a base
RPM table, then (via a dead code path - see below) four independent
saturating-multiplied correction factors - TPS/RPM map, remaining
knock-retard margin, TPS/gear map, intake air temp - into
`var_tvsv_scale_total`, which is rate-limited against the previous
cycle's output (`var_tvsv_117` itself, a feedback term) before a final
RPM-based ceiling clamp.

**Notable finding - dead code, confirmed via the assembled `.lst` and the
startup RAM-clear range, not just a grep:** `dmatx_unk_16A` is written
**only as 0** anywhere in this ROM (one explicit write in
`update_odb_flags`, plus `clear_variables_high`'s startup sweep covering
its address `0x016A`). Two separate pieces of code gate on
`dmatx_unk_16A == 0x0F`:
- The TVSV block's main ECT/THA/RPM/speed override gate (`loc_CEA3`-
  `loc_CEF8`) - this entire branch is unreachable; TVSV always takes the
  "not 0x0F" fallthrough (defaults toward off/minimum) in the code as
  currently understood.
- A `+6` ECT adjustment in the enrichment chain documented two sessions
  ago (`loc_CAC3`) - the opposite effect: that `beq` never fires, so the
  `+6` always applies.

Flagged inline at both sites (`loc_CE86`'s header and `loc_CAC3`) and
cross-referenced. Not 100% provably exhaustive (an exotic indirect-write
mechanism can't be entirely ruled out) but thoroughly checked - same
confidence level as the `unk_51` "no reader" finding two sessions ago.

**Scoped out, not traced:** `loc_D037` (immediately after
`drive_DOUT2_tvsv`) is a different, unrelated battery-voltage/PIM-gated
calculation driving `PORTA.2` - flagged inline, moved to Pending work.

### CPU2 (D151803-9661): remaining 6 sub_ functions resolved
All 11 originally-unresolved `sub_` functions on CPU2 are now named (5 from
the prior enrichment-decay-chain session, 6 this session). Confirmed the
prediction that some would turn out to be library functions shared with
CPU1:

- **`interp_table_pair`** (was `sub_C67C`): shared linear-interpolation
  tail primitive for CPU2's whole table-interpolation family (used by
  `table_rB_fixed_rA_interpolate`, `table_rD_fixed8/16/32_interpolate`,
  and `map_rD_rX_map_interpolate`) - analogous role to CPU1's
  `interp_y_pair` (`sub_C45C`), not a byte-for-byte match but the same
  function-family position.
- **`signed_proportional_update`** (was `loc_C790`): verified
  **byte-for-byte identical** to CPU1's function of the same name
  (`sub_C56D`) - confirmed instruction-by-instruction, not just inferred
  from calling convention. Moves `*Y` a fraction (`B/256`) of the way
  toward `D`, signed.
- **`update_rpm_filter_EA`** (was `sub_CCDF`): smooths `unk_EA` toward `D`
  at ~12.5%/call via `signed_proportional_update`, with a hard reset to
  raw RPM when `dmarx_var_flags_46.0` is clear. `unk_EA` joins `unk_EC`
  (see last session's `update_rpm_smooth_filter`) as a second,
  differently-smoothed RPM tracker - their distinct purposes not
  confirmed. Only reached with `D` already set to an RPM-delta/ratio value
  computed further up in `main_continue` - that computation itself isn't
  traced.
- **`update_ect_enrich_clamp`** (was `sub_CAE9`): refreshes
  `var_unk_ect_table_10A`, the ECT-table clamp ceiling consumed by the
  enrichment chain documented last session (`main_continue_2`'s fuel
  enrichment `min()`). Called periodically (~64ms) alongside
  `update_odb_flags`, not from the enrichment chain itself - closes a
  loose end from that earlier write-up.
- **`factory_selfcheck`** (was `sub_D1DB`) + **`selfcheck_io_pump`** (was
  `sub_D2D3`): a substantial find - CPU2's manufacturing/dealer diagnostic
  self-test entry point. Gated on a diagnostic-connector-shorted-at-
  standstill input combination, dispatches on a CPU1-supplied
  `dmarx_flags2` command byte to either run a full RAM check + ROM
  checksum (verified against `0xAA55`) with a pass/fail PORTA blink
  pattern, or force PORTA/PORTB/DOUT to a fixed output-test pattern.
  `selfcheck_io_pump` keeps I/O and the CPU1 DMA link alive while parked
  in the test loop, mirroring `check_startup`'s own reset-detection check
  so a genuine reset can still interrupt self-test.
- **`update_dmatx_status_flags`** (was `sub_D59A`): packs 10 individual
  status bits (`var_flags_40.6`, `unk_47.2/3`, `var_enrich_flags.5/6`,
  `var_input_bits.5/6/7`, `PORTC.7`, `PORTD_ASRIN.5`) into two DMA bytes
  (`dmatx_unk_169`/`dmatx_unk_16B`) for CPU1. Physical meaning of the
  individual source bits beyond their existing names isn't confirmed.

Full header comments added at each function in the ASM (bit-by-bit for
`update_dmatx_status_flags`, dispatch-table-style for
`factory_selfcheck`). No CPU2 `sub_` functions remain - any further CPU2
work is now `loc_`-level tracing (284 remaining) or full subsystem
write-ups (the TVSV/boost-control section is one such write-up, done in a
later session - see "TVSV boost-control duty-cycle calculation documented"
above), not "find and name the unresolved function" work.

### CPU2 (D151803-9661): first systematic pass - enrichment-decay chain
CPU2 turns out to be far more advanced than the prior "not yet started"
note implied: ~52% of its labels already have meaningful names (683 total,
354 non-generic) and only 11 `sub_` functions remained genuinely
unresolved, from earlier targeted DMA cross-reference work. This session's
first systematic pass on CPU2:

**Resolved 5 of the 11 remaining `sub_` functions**, all clustered around
`calc_ignition_timing`'s periodic (8ms/32ms) gated calls:
- `sub_C9E2` -> `update_rpm_smooth_filter`: maintains `unk_EC`, a
  low-pass-filtered RPM tracker (rounded average, reset to 0 on stall/
  restart elsewhere), and writes `unk_51` (an RPM-deviation magnitude) -
  but **`unk_51` has no reader anywhere in this file**, so its actual
  purpose/consumer is unconfirmed; left unrenamed.
- `sub_CB2E` -> `decay_enrichment_unk_53`, `sub_CBEB` -> `decay_unk_103`:
  multiplicative decay (x0xF0/256, ~93.75%) toward zero when their
  respective variable is nonzero.
- `sub_CB54` -> `decay_enrichment_unk_FE`: multiplicative decay
  (x0xF8/256, ~96.9%), additionally gated on `var_cnt32ms_B2 < 0x3D` or
  `dmarx_pim2`'s high byte `< 0x33`.
- `sub_CBA5` -> `decay_enrichment_unk_100`: the one exception - a linear
  decrement (-3, clamped to 0) gated on an ECT-table lookup vs
  `dmarx_unk_D4`, not a multiplicative scale.

**Discovered the pattern they all belong to**: `main_continue_2` ->
`main_continue_3` -> `loc_CB3D` -> `loc_CB6D` -> `loc_CBCE` (falls into
`calc_params`) is a chain of 5 enrichment-term calculations (fuel
enrichment, warmup enrichment, and 3 ECT/THA-table-lookup terms), each
sent to CPU1 as a `dmatx_*` value. The 4 decay functions above are each
that periodic "fade toward zero when not actively refreshed" companion
for one specific term in the chain - not called from the chain itself, but
from `calc_ignition_timing`'s timer-gated calls. Full write-up as a header
comment above `main_continue_2` in the ASM. The individual terms'
real-world physical meaning (beyond "ECT/THA table lookup, gated on
`var_flags_40.0`/`dmarx_var_flags_46.0`") is NOT confirmed.

**Corrected a pre-existing doc error**: the header comment above
`calc_params` claimed its "Fuel VE / speed-density section" spanned a
single `CC53-CCDA` address range covering 6 items. Verified via the
assembled `.lst` that items 3/4 (`dmatx_ve_corr_map`/
`dmatx_ve_corr_map_tps`) are NOT in that range - they're actually computed
much later, at `CE4E`/`CE67`, deep inside `calc_ignition_timing` itself,
interspersed with unrelated ignition-retard-map and PORTB output logic.
Also flagged a pre-existing internal inconsistency noticed while fixing
this: items 2 and 3 both separately claim to equal CPU1's
`dmarx_word_226`, which can't both be true (not resolved this session).

**Found and scoped out a new large subsystem**: `loc_CE86` onward (right
after the corrected VE section) is a substantial, entirely unexplored
TVSV/boost-control duty-cycle calculation (knock retard, ECT, TPS, RPM,
gear ratio, and limiter flags all feed into it) - matches the "boost
control" role from the architecture notes. Flagged in the ASM and moved to
Pending work below rather than traced this session.

### ramp_limit_inj_pw / ramp_limit_inj_pw_simple full branch trace (was "NOT fully traced")
Full branch-by-branch trace of `ramp_limit_inj_pw`'s nine-way branch
structure (`loc_DBB5`/`DBDE`/`DBF1`/`DC0C`/`DC17`/`DC24`/`DC34`/`DC35`/
`DC37`), re-verifying `mov` direction at every step. Full detail in
fuel_calculation_system.md's "Branch-by-branch trace" section; header
comments added at `ramp_limit_inj_pw`, `ramp_limit_inj_pw_simple`, and
`calc_inj_pw_base` in the ASM.

**Key finding:** `unk_1C0`/`unk_1C4`/`unk_1C6` do not have single fixed
identities (candidate / carried-forward value / ceiling) - each gets
overwritten with a different one of {fresh VE-map candidate,
`var_adc_lambda`, the `unk_1C8` ceiling, a ratio-deviation result,
`var_inj_pw_base`} depending on which branch runs. This explains why no
clean rename was found in earlier passes - there isn't one to find.
`unk_1C2` is the one variable in this cluster with a stable role (a ratio,
nominally `0xCCCD`).

**Correction to a prior-session claim:** `ramp_limit_inj_pw_simple` is
called from `loc_DA58` when `var_adc_lambda` (signed lambda sensor
voltage) is below `0x4D`, not `var_inj_pw_base` as previously documented -
`X` holds `var_adc_lambda` at that point, loaded earlier in the same block
and never reloaded before the gate.

**Partially resolved:** `unk_1C8` (compared as a PW-scale ceiling in
`ramp_limit_inj_pw`) is written from a computation near `loc_E665`
(~`E620`-`E6B0`) that folds in `var_pim2`-derived `dmatx_pim`, confirming
the "PIM/MAP-pressure-linked" read. The rest of that computation's inputs
(`unk_131`, `var_unk_knk_133`/`135`, `var_nv_trim_unk_98`, `unk_1CA`) are
not traced - moved to Pending work below, grouped with the neighboring
`E363`-onward exploration since it's in the same address range.

### CPU1<->CPU2 DMA cross-reference (working copy created, targeted lookup only)
Created 3S-GTE/D151803-9661/Claude/D151803-9661.asm as CPU2's working copy
(exact copy of the buildable D151803-9661.ASM, verified assembles cleanly
with 0 errors - same tasm32 toolchain, same verify_assembly_match.py
workflow applies here too). This was a targeted investigation to resolve
one specific CPU1 open question, not a systematic RE pass over CPU2 - most
of CPU2's ROM is still unexamined.

**DMA buffer offset formula established:** CPU1 and CPU2 share the same
physical inter-MCU DMA buffer, addressed at a fixed offset:
**CPU1_address = CPU2_address + 0xDA**. Confirmed via three independent,
already-cross-named variable pairs (from work predating this journal):
CPU1's `dmarx_max_retard_23B_161` (addr 0x23B) = CPU2's
`dmatx_max_retard_161` (addr 0x161); CPU1's `dmarx_ign_timing_unk_166`
(addr 0x240, not 0x166 despite the name) = CPU2's
`dmatx_ign_timing_unk_166` (addr 0x166); CPU1's `dmarx_unk_241_167` (addr
0x241) = CPU2's `dmatx_unk_167` (addr 0x167). All three give exactly
0x23B-0x161=0x240-0x166=0x241-0x167=0xDA. **Caveat:** this exact offset has
a 1-byte discrepancy for the specific word-sized (2-byte) variables
resolved below (structural/positional matching was used there instead of
the numeric formula) - there's likely a single padding/alignment byte
somewhere in the buffer between the two regions used to confirm the
formula. Worth re-deriving per-region if this matters for other variables.

**Resolved: `dmarx_word_226`/`228`/`22A`'s identities** (was open in
fuel_calculation_system.md) - matched by structural position (three
consecutive word-sized DMA slots on both sides, CPU2 at 0x14D/0x14F/0x151):
- `dmarx_word_226` = CPU2's `dmatx_map_table_unk_14D`: `table_map_unk_C53D`
  lookup indexed by `dmarx_pim2` (MAP), `/32` - a MAP-only VE correction.
- `dmarx_word_228` = CPU2's `dmatx_unk_14F`: `map_map_tps_C51F` bilinear
  lookup indexed by MAP and `dmarx_tps` (TPS), `/32` - zeroed when
  `dmarx_var_flags_46.2` is set (CPU1's idle-debounce flag, relayed back to
  CPU2 via DMA).
- `dmarx_word_22A` = CPU2's `dmatx_unk_151` = `var_ve_x_pim_x_rpm_unk_10C`
  (saturated): CPU2's base VE map (`var_map_ve`, from `map_c006_ve`,
  indexed by RPM and MAP) multiplied by RPM and by MAP again - i.e.
  **VE x MAP x RPM**, the classic speed-density load term.

This confirms CPU1's `calc_inj_pw_base` (chunk D931) is doing a
speed-density base fuel load calculation, with CPU2 supplying the VE map
lookup and two correction tables. Full detail folded into
fuel_calculation_system.md's section 3.

### DC77/DD38/DD59 continuation - periodic I/O debounce / diagnostic phase
Follows directly from D931's fuel-calc hand-off (see below), but turned out
to be a DIFFERENT kind of code: not fuel-pulse-width calculation, but a
periodic I/O-debounce and diagnostic/error-flag-checking phase. Full
write-up folded into the header comment above loc_DC77 in the ASM (this
section was reached via the fuel-calc work, not big enough on its own for
a separate doc file).

Key findings:
- Two RPM-hysteresis blocks for var_flags_4F.0, resetting the same
  var_4ms_cnt_B6/B7 debounce counters used elsewhere (calc_iscv, chunk
  D3A5).
- **A third, independent instance of the var_flags_4E-aliasing trick**,
  this time for `unk_1CF` (not `var_trim_state`) - two confirmed
  short-lived windows: one wrapping a battery/starter (STA) diagnostic
  latch (loc_DD02-DD38), one wrapping a long O2-heater/lambda/coolant
  diagnostic check run (loc_DDB8-DE7B, reached via loc_DD69, itself called
  from a *different, later* point in the main loop - the same short second
  var_trim_state-alias instance that calls loc_DA63). Added a matching
  `unk_1CF_alias` .equ (same technique as var_trim_state_alias) and applied
  it to both confirmed windows.
- Resolved two previously-flagged "not deep-dived" helpers: `sub_DE5A`
  (resets unk_187, sets var_flags_4F.7 if outside [3,0x131)) and
  `sub_DE71` (saturating increment of unk_187) - a simple counter/error-flag
  pair.

**`var_flags_4E_copy_2` investigated and resolved** (was left open at the
end of the previous pass): it is a genuinely different variable from
`var_flags_4E_copy2` (no underscore, used by the var_trim_state alias), but
it is **not** a big overlapping protect-window mechanism. It's a
last-known-good real-value cache, refreshed with the current real
var_flags_4E at multiple independent points (confirmed at chunk CB1E's
loc_CD0B, right after the boost-limit-flag update, and again at
calc_4ms_corrections' loc_EF48) and consulted/restored elsewhere (right
after chunk C9DA, and at loc_DF2A in the DC77/DD69 diagnostic phase)
whenever code needs a valid real value that isn't otherwise fresh - e.g.
because var_flags_4E is mid-excursion for the var_trim_state or unk_1CF
aliases. Confirmed by reading the code right after loc_DF2A's restore: it
uses var_flags_4E.7 exactly per its documented meaning (boost-limit
error), consistent with a genuinely-real value. **Does not affect** the
var_flags_4E interpretations in chunks C9DA/CB1E/D1DD/D3A5, calc_iscv, or
calc_4ms_corrections - all confirmed still correct.

### D931 fuel pulse-width calculation (was flagged "not deep-dived" earlier this session)
Full documentation in fuel_calculation_system.md.

The core base injector pulse-width calculation: open-loop/closed-loop path
selection (init_pw_closed_loop/init_pw_open_loop), a VE-map candidate calc from CPU2 DMA words
(dmarx_word_226/dmarx_word_228/dmarx_word_22A) scaled by a fixed constant
(0x1EB8), and a ~0.8x-per-step (0xCCCD/0x10000 Q16) rate-limited blend
toward that candidate (ramp_limit_inj_pw/ramp_limit_inj_pw_simple, both flagging overflow via
set_knock_sensor_err_flag). Result: var_inj_pw_base (was unk_1BE), fed
downstream to the per-injector dead-time/battery driver logic already
documented under "Injector system".

**Update - VE-map candidate calc mechanics resolved:** after correcting the
mov-direction misreading (see below), re-traced the D998-DA10 candidate
calc and found the "why" behind its odd-looking high-word substitutions:
mult_rDrX auto-saturates its own D output to 0xFFFF on overflow (confirmed
by reading its body), and this code deliberately overrides that lossy clip
with the true high-word magnitude instead, preserving proportional
information rather than losing it to a flat pin. A deliberate, sensible
technique, not an oddity - see fuel_calculation_system.md's updated
section 3 for the full corrected trace.

**Major finding - var_flags_4E/var_trim_state aliasing:** for roughly
address range 0xD931-0xE380 (chunks D931, DC77, DD38, DD59, E112, and the
start of E363), var_flags_4E is deliberately overwritten to hold
var_trim_state's value and used as a scratch register, reusing the
existing tbbc/tbbs/setb/clrb-on-var_flags_4E instruction encodings against
var_trim_state's bits instead of compiling separate code. Confirmed by the
snapshot-at-entry (var_flags_4E_copy2/unk_1D8), the commit-without-restore
at loc_DC77, and an identical short-lived instance later
(var_trim_state -> var_flags_4E -> jsr loc_DA63 -> var_flags_4E ->
var_trim_state) right before the real var_flags_4E is finally restored
around address E37F (chunk E363). **Any "var_flags_4E" bit-test in this
address range means var_trim_state, not flags_4E's documented bits** - see
fuel_calculation_system.md for the full evidence trail. This doesn't
affect any subsystem documented earlier this session (calc_iscv,
injector_warmup, and the C9DA/CB1E/D1DD/D3A5 chunks are all at addresses
below 0xD931).

Renames: unk_1BE -> var_inj_pw_base (clamp range matches known injector PW
units elsewhere in the ROM). unk_1C0/1C2/1C4/1C6/1C8/1BD left unrenamed -
participate in the ramp-limiter but their precise distinct roles weren't
pinned down with confidence.

**Readability alias for the trim_state aliasing:** added
`var_trim_state_alias` (`.equ var_flags_4E`, same address, .equ referencing
another label - zero bytes changed, verified via verify_assembly_match.py)
next to var_flags_4E's declaration, and applied it to every reference
confirmed this session: calc_inj_pw_base's own body, reset_pw_ramp_limiter/ramp_limit_inj_pw/ramp_limit_inj_pw_simple,
and loc_DA63's full body (through locret_DB74, including sub_DB75/
sub_DB77 - discovered this session to be a SEPARATE short-lived instance of
the same trick, called from much later in the main loop, not part of
D931's direct continuation). Also found var_cnt_6A's consumer while
tracing loc_DA63: loc_DB34 gates trim_state.5 on "var_cnt_6A >= 3 ticks".
NOT yet renamed: loc_DC77's body past its entry commit, and chunks
DD38/DD59/E112/start-of-E363 - confirmed to be the same alias (no
var_flags_4E_copy2 restore happens before ~E37F) but not read/traced, so
left as "var_flags_4E" rather than renamed blind.

### The 5 remaining divide_d_by_x chunks (D9C9/D1DD/D3A5/CB1E/C9DA)
All five chunks from the old pending-work table were traced this session.
**Correction:** "D9C9" doesn't exist as an address in the ROM - its listed
size (306 bytes) exactly matches chunk `D931`, so that's almost certainly
what was meant. All five are, as the header comment above `divide_d_by_x`
already noted, unrelated code blocks IDA misattributes as chunks of that
function - not part of division at all. Header comments were added at each
chunk's entry point in the ASM; this is the narrative summary.

**C9DA (294 bytes) -> falls into CB1E:**
- A debounce chain (`var_io_input1` bits 2/3, both undocumented signals,
  plus startup timing) that sets `var_ignition_flags.6` (previously
  undocumented - added to ignition_system.md) and `var_flags_46.6`. This
  resolves the open question left in idle_control_system.md: bit 6 gates
  whether the ISCV runs closed-loop (`calc_iscv`) or a fixed override -
  see chunk D3A5 below. Also sets `var_flags_4E.7` in this path, though
  that bit is documented elsewhere as "boost limit exceeded" - not
  confirmed whether that's the same condition or bit reuse.
- Calls sub_E454 (fuel enrichment scaling, confirmed), sub_E551, loc_FC38,
  sub_D2C5 (NV trim validation, see D1DD below) - only sub_D2C5 was
  traced.
- The overrun/deceleration fuel-cut decision feeding `injector_warmup`
  (already documented via injector_warmup's own header comment).
- `var_lambda_state`-gated calls to `inj_overrun_end`/`inj_overrun_end_2`
  (siblings of injector_warmup, not deep-dived).

**CB1E (373 bytes) -> falls into check_clear_speed_limiter:**
- Closed-loop mode entry/exit (`var_flags_4E.1`) from injector 1 pulse
  width and `var_lambda_state`.
- A MAP-vs-RPM boost/overrun latch (`var_flags_4E` bits 3/4), debounced
  over ~976ms.
- **The rev limiter** (`reset_rev_limiter`, `var_rev_limit_rpm`,
  `var_limiter_flags.6`): default cut is `0x9400` (RPM*5.12 high byte
  0x94 = ~7400rpm). Alternate values in comments next to the hardcoded
  constants are per-tuner: `9e00h` (~7900rpm, "Marf") and `a5h`
  (~8250rpm, "Jon") - these names match the "Marf ECU" and "Jon ST205
  ECU" person-named folders elsewhere in this repo. Confirms the repo's
  tuning history is partially preserved as commented-out alternates
  directly in the disassembly, not just in the separate ECU folders.

**D1DD (232 bytes) -> falls into loc_D2D2 (calc_4ms_corrections call site):**
- Periodic counter increments plus sub_C91A, sub_CB00 (lambda_state
  decay), ramp_misfire_correction, sub_DE71 (not deep-dived).
- A **second closed-loop lambda trim system** (`closed_loop_control`
  label), distinct from the RPM/MAP-zone `nv_afr_trim_base` system in
  calc_4ms_corrections' chunk CE6C. Gated on ECT 83-104C, off-idle,
  RPM<3200, battery>=11.4V, and `var_trim_state==4`. Accumulates O2
  sensor polarity into `var_lambda_count_unk_6C` over 17 samples
  (`unk_6B`), then nudges `var_nv_trim_unk_96` by +/-1 via a
  majority-style threshold. Not renamed - didn't confirm what
  specifically distinguishes this from the zone-based AFR trim (e.g.
  "cruise" vs "part-throttle"), worth a follow-up.
- `sub_D2C5`: validates `var_nv_trim_unk_96` against `nv_96_limits` and
  wipes ALL NV RAM via `clear_nv_ram` if out of range - same defensive
  pattern as the AFR trim validation in chunk CE6C.

**D3A5 (228 bytes) -> ends at the var_iscv_pwm store:**
- The ISCV **fixed-opening override** layer - runs right after `calc_iscv`
  every 4ms and can substitute a fixed `0x0200`/`0x0300` pulse width
  instead of `calc_iscv`'s computed `var_iscv_19D`, during startup or
  various "not ready" flag conditions. This significantly extends
  idle_control_system.md, which previously only covered `calc_iscv`'s own
  output and didn't document that it can be bypassed entirely. Folded
  into that doc directly (new "Fixed-Opening Override" section).

**D931 (306 bytes, journal previously mislabeled "D9C9") -> falls into loc_DC77:**
- Closed-loop mode entry (`var_flags_4E.1`, mirrors CB1E's logic) and what
  looks like the **core fuel-injection base pulse-width calculation**:
  combines CPU2 VE/fuel-map DMA words (`dmarx_word_226/228/22A`) with
  `var_lambda_integrator` to produce `unk_1BE`/`unk_1C0`/`unk_1C4`. Calls
  init_pw_closed_loop/init_pw_open_loop (open-loop vs closed-loop variants?), ramp_limit_inj_pw,
  ramp_limit_inj_pw_simple. **Not deep-dived** - this looks comparable in importance to
  calc_4ms_corrections or calc_iscv and deserves its own dedicated session
  rather than the breadth-first treatment given to the other four chunks.

### calc_iscv — idle speed control (was untouched, "Not yet started")
Full documentation in idle_control_system.md.

Computes the ISC valve duty target every 4ms: sums several flare/enrichment
terms into a target idle RPM, compares against actual RPM for an error term,
runs that error through twin RPM-band tables (table_iscv_rpm_C357/C361) to
update two running values (var_iscv_target_base, unk_1A1), adaptively learns
var_idle_trim into NV RAM once idle is stable, and finishes with a bilinear
map (table_idle_C2FE) to produce var_iscv_19D. Downstream (divide_d_by_x)
combines this with battery dead-time compensation into var_iscv_pwm, which
drive_dout1_iscv turns into a CPR1/DOUT.1 timed pulse - the same
timer-compare PWM pattern used for ignition (CPR0) and injection (CPR4/6/7/5).

Renames: var_iscv_unk_195 -> var_iscv_target_rpm, var_iscv_unk_19B ->
var_iscv_target_base (both confidently justified by tracing their sole
producers/consumers; see idle_control_system.md). Several other unk_
variables (unk_1A0/1A1/1A3/1A5/1A7/9E/E2) are understood at the "some
flare/ramp/compensation contribution" level but not renamed - see that doc's
Open Questions section for specifics worth revisiting.

**Correction to a prior-session assumption:** while adding inline comments I
initially guessed `var_flags_46.6` meant "sensor error/limp mode" (by
analogy with bit 7, which genuinely is the documented ISC sensor error flag).
Tracing bit 6's only writer (divide_d_by_x:loc_CA11) shows it clears based on
post-start timing counters, not an error condition - comments were corrected
to avoid asserting a specific meaning. Worth remembering: bit meanings within
the same flag byte are not analogous just because they're adjacent.

### divide_d_by_x (was sub_C59B)
- 16-bit unsigned software division: D = D / X
- 962 occurrences renamed throughout file
- Algorithm: normalise (count leading zeros into Y), restoring long division, de-normalise
- IDA incorrectly groups 30+ engine management chunks as "function chunks" of this function
- Returns: D = quotient, C = 1 on overflow (saturates to 0xFFFF)

### Chunk C667 — reset_vector / startup sequence
- reset_vector: hardware init (ASR timers, ASR2=0x81DE DMA RX, ASR3=0x9200 DMA TX, ports, serial flush, stack)
- clear_variables: byte-fill unk_40..unk_7F, word-fill var_diag_errors_4..dmarx_ign_advance_lo
- loc_C67A: software defaults (flags, counters, sensor defaults, ignition limp mode, idle trim restore)
- adc_start: enable serial RX interrupt, kick ADC phase 1, spin-wait at loc_C718 until complete, send first DMA frame
- loc_C749: 4ms main loop entry / watchdog re-entry point

### iv6_4ms_process
- Triggered by IV6 software interrupt every 4ms from int_4ms_watchdog
- Every tick: increment counters, read I/O, ISC relay health monitoring, start DMA, init_ne_on_start, sub_EBF3, alternating knock processing, speed/gear update every 344ms
- Timer sub-slots via TIMER bits after >>3:
  - Every 8ms: increment secondary counters + copy DMA TX
  - Every 16ms: clear var_schedule_flag_41.7
  - Every 32ms: clear var_schedule_flag_41.6 or bits 4+5
  - Every 64ms: clear var_schedule_flag_41.7 (separate slot)
- Renames: dmarx_iscv_duty (was dmarx_unk_242_168), var_iscv_relay_cnt (unk_AB), var_iscv_error_cnt (unk_AC)

### update_idle_timing_ramp (was sub_EBF3)
- Tiny 5-instruction function called every 4ms
- Ramps unk_15E up by 4 per call, saturated at 0xFF
- Only when var_flags_4E_copy_1D3 == 0x08 (only idle bit set)
- Feeds into calc_ign_timing_min to compute var_ign_timing_min

### calc_4ms_corrections (was sub_EA22)
Main 4ms ignition and fuel correction function. 202 references. 6 IDA chunks.

Sections:
1. Dwell: var_ign_dwell_offset = battery * RPM / 32, var_ign_dwell_min from RPM table
2. Closed loop enable: 368ms post-start, RPM>1000, speed>3kph, ECT>70C, battery>8.6V
3. Idle/overrun detection via IDL signal, gear ratio, RPM slope
4. Open-loop ignition correction (var_open_loop_ign_corr) integrates toward 0x80
5. Per-cylinder RPM deviation -> var_ign_advance_trim (misfire detection)
6. Knock retard assembly -> dmatx_knock_retard
7. var_ign_advance_max ramp
8. var_ign_knock_retard_base and var_ign_cold_advance

Helper functions renamed/documented:
- decay_overrun_advance (sub_EA97): var_overrun_advance -= 9, clamp 0
- clamp_overrun_advance (sub_EAA3): return min(var_overrun_advance, 0x2B)
- calc_ign_timing_min (sub_EB57): compute var_ign_timing_min from RPM/idle/knock
- ramp_misfire_correction (sub_EC07): var_cyl_rpm_filtered += 4 when misfire active
- check_clear_speed_limiter_tps (sub_EE93): clear limiter when speed/TPS conditions met
- check_clear_speed_limiter_rev (sub_EEA8): clear limiter (rev variant)
- check_set_overrun_flag (sub_EF2C): set var_flags_4E.6 (overrun fuel cut)

Key variable renames (calc_4ms_corrections):
- unk_A3         -> var_overrun_advance
- unk_155        -> var_lambda_ign_corr
- unk_156        -> var_open_loop_ign_corr
- unk_15C        -> var_ign_corr_combined
- unk_161        -> var_cyl_rpm_delta
- unk_162        -> var_cyl_rpm_filtered
- unk_163        -> var_rpm_ne_sum3
- unk_165        -> var_rpm_ne_sum3_prev
- unk_167        -> var_rpm_div25_prev
- unk_178        -> var_cyl_proc_idx
- unk_179        -> var_ne_sum3_prev
- unk_17B        -> var_cyl_rpm_dev (4-element array)
- dmarx_unk_23C  -> dmarx_lambda_trim
- dmarx_unk_239  -> dmarx_fuel_ign_corr
- dmatx_unk_216  -> dmatx_ign_corr_cpu2
- unk_1D2        -> var_flags_4E_saved
- unk_1D4        -> var_flags_4E_temp

### Chunk CE6C — lambda closed-loop fuel trim
576 bytes. Full documentation added.

Sections:
1. ECT warmup flag: var_flags_4E.2 via hysteresis at 69/75C (opcode trick 0x8C)
2. Overrun fuel multiplier (var_overrun_fuel_mult): RPM table lookup when decel conditions met
3. Acceleration enrichment: var_accel_enrich = TPS_table * var_overrun_fuel_mult
4. Open/closed loop selection: comprehensive gate list including ISC duty, ECT, PW, battery, trim_state
5. Lambda integrator (var_lambda_integrator, 16-bit, 0x8000=stoich): integrates via step tables
6. NV trim validation: wipes nv_afr_trim_base..end if any cell out of bounds
7. NV AFR trim update: select cell by RPM/MAP zone, adjust +/-1, write to PRAM, propagate

Helper functions:
- update_lambda_avg (sub_D187): var_lambda_avg = (old+new)/2, increment trim counters
- write_rB_nv_ram: write trim B to PRAM cell at X with delta correction
- read_nv_afr_trim (sub_D1A1): read PRAM trim by PIM zone, interpolated

Key variable renames (chunk CE6C):
- word_62         -> var_lambda_integrator
- unk_11F         -> var_overrun_fuel_mult
- unk_120         -> var_accel_enrich
- unk_60          -> var_lambda_state
- unk_61          -> var_lambda_byte
- unk_64          -> var_lambda_avg
- unk_1D1         -> var_trim_state
- nv_unk_trim_86  -> nv_afr_trim_base
- nv_unk_trim_94  -> nv_afr_trim_top
- nv_unk_trim_95  -> nv_afr_trim_end
- unk_121         -> var_lambda_step
- unk_123         -> var_lambda_step_lo
- unk_124         -> var_lambda_step_hi
- var_unk_trim_67 -> var_trim_cell_idx
- var_cnt_trim_69 -> var_trim_stable_cnt
- dmarx_enrich_232/233, dmarx_warmup_enrich, dmarx_idle_enrich, dmarx_fuel_trim_231

### injector_warmup (sub_CD68) — now commented
Fires one batch injection pulse (via injectors_batch_update), gated on RPM
being past the cranking/stall band, an idle-debounce latch (unk_44.0), and
the throttle-closed debounce timer having settled (var_flags_46.2 clear).
Picks between an ECT-indexed table (normal case) or an RPM-indexed table
(when var_limiter_flags.0 shows an overrun fuel-cut is being recovered from
this cycle, per the caller at divide_d_by_x:loc_CAD4/loc_CADD) before scaling
the result down (divide_rD_64) and firing the injectors.

**Correction to a prior-session comment:** `var_flags_46.0` was labeled
"engine running flag" (in `injector_cold_start`'s header). Tracing its sole
writer (divide_d_by_x:loc_C93D) shows it is actually set when RPM < 200 and
cleared when RPM >= 400 (200-400 is a hysteresis band) — i.e. it's a
**"RPM low" / cranking-or-near-stall** flag, opposite polarity from what
"engine running" implies. Comments at loc_C93D/C945 and in
injector_cold_start's header/body were corrected to match. This also matters
for injector_warmup, whose first gate is the same bit.

**Not fully verified:** `table_ect_unk_C1D8`'s header byte (06h) implies 3
(x,y) pairs but 4 pairs are laid out after it in ROM — the exact table
layout/consumption by `table_ect_pair_interpolate` for this specific table
wasn't reconciled and is worth a closer look later.

**Tooling note:** `D151803-9651.asm` (and presumably other IDA-exported
`.ASM`/`.asm` files in this repo) contain stray single control bytes (0x18)
immediately before the trailing xref type-letter in `; CODE XREF: ...+Nj/p/r/w/o`
comments (e.g. `loc_CAD4` + `0x18` + `p`). These are invisible when the file
is read normally but break naive exact-string edits. When editing a line that
ends in one of these xref comments, match only up to just before the final
offset+type-letter, or avoid touching that line's trailing comment at all.

### check_limiters_active / check_limiters_active_2 (near injector_drive, ~F418)
Renamed from sub_ defaults and inline-commented (fuel-cut/limiter-flag check
helpers called from injectors_batch_update and injector_update). Not yet
given a full header-block writeup in the gold-standard style - the inline
comments explain each instruction but the overall purpose/caller contract
hasn't been folded into a subsystem doc. Worth a follow-up pass.

### Tooling: `.equ` directive support added to d8x_assembler
`asm_d8x.py`/`directive.py` had no handler for `.equ` at all - an unrecognized
directive was reported as a per-line error, but because `HandleLabel` always
ran *before* directive dispatch and unconditionally bound the label to the
current PC, the label still ended up defined (just wrong: PC instead of the
aliased variable's address). This silently broke `var_trim_state_alias` and
`unk_1CF_alias` (see "D931 fuel pulse-width calculation" above) - both
resolved to wherever they happened to sit in the RAM layout instead of
`var_flags_4E`'s real address, which then surfaced as confusing "No
instruction encoding" errors on the `setb`/`clrb`/`tbbc` lines that use them
(bit instructions require the aliased target's real direct-page address).
**This means the earlier claim that these aliases were "verified via
verify_assembly_match.py" was not actually reproducible** - fixed now by
adding a proper `HandleEqu` (binds the label to the parsed expression's
value via `SetLabel`, not PC) and teaching `asm_d8x.py`'s line handler to
withhold the normal PC-based label assignment on an `.equ` line. All 70
existing unit tests still pass, and Claude/D151803-9651.asm now both
assembles with 0 errors and matches the buildable D151803-9651.ASM with 0
real edit regions via verify_assembly_match.py - the aliasing technique is
now genuinely confirmed byte-safe.

---

## Previously completed (prior sessions)

### Maths/interpolation library
- table_pair_interpolate family (1D piecewise linear)
- table_rD_fixedN_interpolate cascade, table_rD_clamp
- table_advance_y_to_entry (sub_C459), interp_y_pair (sub_C45C)
- 2D map: map_rD_rX_interpolate (bilinear)
- Divide: divide_rD_N, divide_rD_N_signed, divide_rD_N_saturate
- clamp_rD_FF, clamp_rB, clamp_rD
- Multiply: mult_rDrX, mult_rDrX_saturate, mult_rBrX2, mult_rBrX, mult_rArX
- scale_d_by_a_frac (sub_C539), signed_proportional_update (sub_C56D)

### NE interrupt & RPM (int_vector_e_ne, calc_rpm)
- 24 NE pulses/rev. ne_count bits 3..0=position(0..5), bits 7..4=cylinder(0x00/10/20/30)
- G2 sync at 0x35, G1 sync at 0x15. Ring buffer var_ne_0/1/2
- var_ne_sum3 = 3 pulse periods = 45 degrees
- RPM = 1,875,000/var_ne_sum3. var_rpm_x_5p12 = RPM*5.12. var_rpm_div_25 = RPM/25

### Knock sensor system (knock_mcu_update, knock_processing, knock_retard_decay)
Full documentation in knock_sensor_system.md
- knock_mcu_update: bit-banged PORTB protocol, position dispatch, 3-bit knock level decode
- Positions 0/1: read knock data bits. Position 2: decode via 3xshr+rorc
- V clear=knock, V+C set=no knock, V+C clear=borderline
- knock_retard_decay: -2 per 4ms, resets nv_table_knock_info to 0x9A9A on error
- Per-cylinder retard in nv_table_knock_info[3] (PRAM)

Key renames:
- var_knock_unk_1B3 -> var_knock_retard
- var_knock_unk_1B8 -> var_knock_event_cnt
- var_knock_unk_1B9 -> var_knock_retard_prev
- var_knock_unk_1BA -> var_knock_retard_max
- var_knock_unk_1BB -> var_knock_retard_prev2
- var_knock_unk_1BC -> var_knock_cyl_idx
- dmarx_knock_unk_23A_160 -> dmarx_knock_retard_cpu2
- table_knock_C395 -> table_knock_retard_step
- table_knock_rpm_C39B -> table_knock_rpm_bands
- sub_F6C5 -> knock_retard_decay

### ADC system (int_vector_1_serial_rx + 14 handlers)
Full documentation in adc_system.md

Phase 1: sequential scan commands 0x00-0x0D (14 channels)
Phase 2: 8-slot schedule via table_adc_ch_normal or table_adc_ch_trac
- slot 0: SKIP, slot 1: O2 heater/battery, slot 2: LOW-PRI, slot 3: DIAG
- slot 4: SKIP, slot 5: TRAC-TPS, slot 6: TPS, slot 7: DIAG
Low-pri groups (indexed by adc_idx>>3): ECT, THAM/THA, Battery, ISCV channels

Handler renames:
- adc_handler_unk_1CB -> adc_handler_iscv_pos
- adc_handler_unk_1CC -> adc_handler_iscv_fb
- adc_handler_unk_1CD -> adc_handler_iscv_3
- adc_handler_unk_1CE -> adc_handler_iscv_4

### Ignition system (int_vector_9_ignition + iv6_ne_process + helpers)
Full documentation in ignition_system.md

- CPR0 compare register fires int_vector_9_ignition to toggle DOUT.0 (coil)
- DOUT.0=0: coil charging, DOUT.0=1: spark fired
- 65ms emergency fixed dwell (CPR0 += 16250) when no pending on-time
- Timing units: ~0.5 deg/count, 0x2B = 0 BTDC reference
- ignition_timing_to_cpr (sub_F263): degrees -> CPR units via ne_sum3/45

Key renames:
- sub_F229 -> ignition_schedule_off
- sub_F240 -> ignition_schedule_on
- sub_F263 -> ignition_timing_to_cpr

### Injector system
- table_injector_control: 3-row [LDOUT bit, LDOUT mask, CPRn] for 4 injectors
  DOUT.4/6/7/5 and CPR4/6/7/5 (injectors 1-4)
- injector_drive: 52us minimum, battery dead-time, accumulated injection, 180deg max
- injectors_batch_update: all 4 simultaneously (cold start, throttle pump, overrun)
- injector_cold_start: ECT/THA temperature gates, 5ms vs 10ms pulse
- async_throttle_inject: TPS delta>=14, RPM<4000, ECT-based pump shot/32
- calc_inj_phase_lead (sub_F375): NE advance count = 34 - f(PW*RPM)
  table_inj_phase_trim (sub_F375): 4 entries all 0x0D

---

## Pending work (next targets)

Split by which CPU/ROM each item belongs to - CPU1 (D151803-9651) and
CPU2 (D151803-9661) are physically separate chips with separate address
spaces; addresses/chunk names below are only meaningful within their own
ROM.

### CPU1 (D151803-9651)

**Functions renamed but not commented:**
- calc_ign_timing_min (sub_EB57)
- check_limiters_active / check_limiters_active_2 (near injector_drive) -
  renamed and inline-commented, but no header-block writeup yet

**Not yet started:**
- **sub_E551 (~E551-E6B0+, 350+ bytes) - a substantial, entirely
  uncharacterized function**, scoped out while chasing unk_1C8's producer
  chain (its immediate write site, loc_E6A8, sits just past this
  function's end and folds in a dmatx_pim/var_pim2-linked value that
  traces back into sub_E551's output). Calls sub_E767, uses TPS delta
  (get_tps_unk/var_tps_delta), runs a signed_proportional_update loop
  against var_unk_knk_133, and calls set_knock_sensor_err_flag/
  check_knock_sensor_err_flag - looks like its own knock/PIM-linked
  limiting calculation (possibly boost/overpressure-related), not a small
  helper. Recommend a dedicated session, same treatment as D931 got. See
  fuel_calculation_system.md Open Questions for detail. **Related:**
  loc_FC38 (also called from chunk C9DA, alongside sub_E551) - not
  deep-dived either, worth tackling in the same pass.
- loc_E112 onward, and the start of chunk E363 up to the restore point
  around address E37F - continuation of the DC77/DD38/DD59 diagnostic
  phase; loc_DD59 jumps directly to loc_E112.
- The second closed-loop lambda trim system in chunk D1DD
  (var_nv_trim_unk_96/unk_6B/var_lambda_count_unk_6C) - distinguish its
  purpose from the zone-based nv_afr_trim system
- loc_DA63's lambda_avg/lambda_integrator adjustment logic (traced/renamed
  for the alias, but not characterized - looks like yet another distinct
  lambda-trim mechanism, see fuel_calculation_system.md Open Questions)

### CPU2 (D151803-9661)

All functions are now named (verified: every jsr/bsr call target,
indirect/computed call target, and interrupt vector table entry resolves
to a meaningful name - see "loc_ sweep" above). What's left is prose
documentation and a few specific loose ends, not "find the function" work.

**Not yet started:**
- **serial_debug_check's full protocol framing** - discovered while
  resolving unk_51's consumer: a generic "read arbitrary RAM word by
  index" debug protocol over the K-line serial link (see that function's
  header comment). The overall existence and purpose is established, but
  the exact byte-by-byte framing (how the 2-byte index accumulates across
  4ms ticks, what SSD.6 distinguishes) isn't traced. Worth a dedicated
  subsystem doc, similar treatment to adc_system.md.
- **dmarx_unk_D6's physical meaning** - a signed byte from CPU1, used
  only by its sign on CPU2 (loc_C9BC's RPM filter gate, and
  loc_CCA0's dmatx_unk_162 gate). Not traced on CPU1's side - what CPU1
  computation produces it, and what a negative value represents, is
  unknown.
- **Broad prose-documentation gap**: ~52% of labels have meaningful
  pre-existing names and all functions are resolved (see above), but most
  of the 284 remaining loc_ labels' surrounding code lacks the
  gold-standard header-comment treatment - fuel VE, ignition timing base,
  the enrichment-decay chain, TVSV boost control, the warning-debounce
  phase, factory_selfcheck, the RPM-smoothing helpers, and (as of this
  session) the boot sequence/main control-flow backbone
  (reset_vector/clear_variables/loc_C88E/main_loop/calc_rpm) have real
  write-ups so far - most of what's left is main_continue_2 onward's
  neighbors not yet covered, the TVSV/warning-debounce area's own
  neighbors, update_odb_flags's full body, and the serial/ADC/interrupt
  handling not yet covered (int_vector_e_asr2/iv6_ne_process,
  int_vector_0, int_vector_6_sw_int, int_vector_4_kph/update_spd,
  copy_serbus_rx, check_io_inputs, generate_vf_PORTA_4). Leverage existing
  renames (dmarx_pim2/dmarx_tps/dmarx_ect/var_map_ve/etc.) rather than
  re-deriving. See "CPU1<->CPU2 DMA cross-reference" below for the
  targeted (not full-pass) DMA lookup work done earlier.

---

## Key variable reference

| Variable | Description |
|----------|-------------|
| var_ne_sum3 | Time for 45 crank degrees (3 NE pulses) in 4us units |
| var_rpm_x_5p12 | RPM * 5.12 |
| var_rpm_div_25 | RPM / 25 |
| var_pim2 | Scaled MAP/boost pressure (0 = 1 bar) |
| var_tps | Throttle position (16-bit, 0 if error) |
| var_ect | Coolant temp (XOR-inverted 16-bit, high=hot) |
| var_tha/tham | Air temps (XOR-inverted) |
| var_inj_battery_adjust | Injector dead-time battery compensation |
| var_knock_retard | Per-cylinder knock retard integrator |
| var_ign_cold_advance | Fixed advance for cold/limp mode |
| var_ign_advance_max | Maximum allowed ignition advance |
| var_ign_timing_min | Minimum ignition advance (idle timing floor) |
| var_gearing | RPM*1.28/speed_kph (gear ratio metric) |
| var_schedule_flag_41 | Injection scheduling gate flags (bits 4-7) |
| var_lambda_integrator | Closed-loop lambda integrator (0x8000=stoich) |
| var_lambda_avg | Rolling average of last two O2 readings |
| var_overrun_advance | Decel ignition advance (0..0x2B, decays -9/4ms) |
| nv_afr_trim_base | Base NV AFR trim cell (PRAM) |
| var_flags_4E | Operating mode flags (bit1=CL, bit2=warm, bit3=overrun/idle) |
| var_flags_4F | Lambda mode flags (bit0=lean, bit1=avg valid, bit5=CL enable) |
| var_flags_46.0 | "RPM low" flag: set RPM<200, cleared RPM>=400 (cranking/near-stall, hysteresis 200-400) |
| var_flags_46.2 | Throttle-closed (IDL) debounce flag: set once IDL has been closed for an ECT-dependent settle time |
| var_limiter_flags.0 | Overrun/deceleration fuel-cut active flag |
| var_iscv_target_rpm | Target idle RPM (var_rpm_x_5p12 units), sum of flare/enrichment terms |
| var_iscv_rpm_cmp_197 | Idle RPM error: (actual - target_rpm)/16, saturated |
| var_iscv_target_base | Persistent ratcheted ISCV duty baseline, seeded from nv_idle_trim |
| var_iscv_19D | Final ISCV duty target from calc_iscv, before battery compensation |
| var_iscv_pwm | Final ISCV PWM pulse width (timer units), consumed by drive_dout1_iscv |
| var_rev_limit_rpm | Current rev-limiter RPM cut threshold (RPM*5.12 units), default 0x9400 (~7400rpm) |
| var_ignition_flags.6 | Debounced from var_io_input1 bits 2/3 + startup timing (chunk C9DA); feeds var_flags_46.6 |
| var_nv_trim_unk_96 | A second, non-zone-based closed-loop lambda trim (NV RAM), distinct purpose from nv_afr_trim_base not yet confirmed |
| var_inj_pw_base | Working base injector pulse-width (was unk_1BE), clamped 0x0000-0x0500 |

---

## Architecture notes
- CPU1 (D151803-9651): real-time I/O — ADC, ignition, injectors, idle
  control, closed-loop lambda trim (var_lambda_integrator/nv_afr_trim_base)
- CPU2 (D151803-9661): fuel/ignition maps, boost control (TVSV duty-cycle
  calculation). **Correction**: does NOT do lambda calculations itself -
  only reads dmarx_adc_lambda (relayed from CPU1) to pack a rich/lean
  status bit into an OBD diagnostic output byte (update_odb_flags). All
  closed-loop lambda trim ownership is CPU1's.
- Inter-CPU: ASR2 (DMA RX 0x81DE) / ASR3 (DMA TX 0x9200), 4ms frame rate
- TIMER resolution: 4us per count (TIMERC/8)
- NE pulses: 24 per revolution (6 per cylinder x 4 cylinders)
- The D8X "enhanced" variant (used here) has 8 CPRs (CPR0-7), ASR2/3 = serial DMA
- **Variable-aliasing code-reuse trick:** watch for a "snapshot real var X to
  scratch, overwrite X with variable Y's value, run a block of tbbc/tbbs/
  setb/clrb-on-X code, commit X back to Y, (eventually) restore X from
  scratch" shape anywhere in the main loop. This reuses one variable's bit
  manipulation instruction encodings against a completely different
  variable to save ROM space, and it means bit-tests on the aliased
  variable inside that span do NOT have their usual documented meaning.
  Confirmed once so far: var_flags_4E aliased to var_trim_state across
  roughly 0xD931-0xE380 (see the D931 fuel pulse-width write-up above) -
  but the same shape could appear elsewhere and hasn't been searched for.
  A `.equ`-based readability alias (`var_trim_state_alias`, same address,
  zero bytes changed) was added and applied to every reference in this
  span that's been read/traced so far - use the same technique (a new
  `.equ` label at the aliased variable's address) if another instance of
  this pattern turns up elsewhere.
- **`mov` operand direction is reversed from `ld`/`st`:** `mov src, dest`,
  not `dest, src` — e.g. `mov x, d` means `D = X`, not `X = D`. Confirmed
  two ways: the technical reference's instruction table states this
  explicitly (`mov x, d` = "D ← X"), and the gold-standard
  `knock_mcu_update.ASM` annotates `mov a, b` as "B = var_knock_info" where
  A held that value - both independently agree. This is very easy to
  misread (it was misread in this session's own calc_iscv comments before
  being caught and fixed - see docs/fuel_calculation_system.md's "mov
  direction" note for the full writeup and the corrected VE-map candidate
  trace it affected). **Pre-existing comments elsewhere in the ROM (from
  earlier sessions) show the same reversed-notation mistake** - e.g.
  `mov s, x` commented "SP = X" in several places in the shared math
  library, when the correct reading is "X = SP(+1)". Not corrected this
  session (would need a dedicated audit pass across the whole ROM) - but
  don't trust an existing `mov`-related comment at face value; re-derive
  from the instruction table when it matters.
