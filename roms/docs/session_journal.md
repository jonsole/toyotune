# D151803-9651 Reverse Engineering Session Journal

## Session overview
Reverse engineering of Toyota 3S-GTE ECU CPU1 ROM (Toshiba D8X / Denso 8X MCU).
Working file: D151803-9651.ASM (IDA Pro disassembly, CP437 encoding - see
"Claude/ working copies converted to UTF-8" below -, \r\n line endings)

---

### Sibling-ROM port: D151803-9651 (CPU1) -> D151804-0461
Scoping pass to see whether the 9651 RE work transfers to D151804-0461 — same
3S-GTE, different car; 9651 is air-to-air intercooled, 0461 has a water
chargecooler with an ECU-controlled pump. It transfers well. Method and
results below; a `Claude/D151804-0461.asm` working copy now exists with the
first batch applied.

**Method.** Names cannot be ported by address — the two RAM maps genuinely
differ. Comparing the 125 hand-named symbols the two files already share
gives a piecewise shift: delta 0 below ~0x0B3, -4 across ~0x0B4-0x0CC, -6
across ~0x0CE-0x14C, -8 across ~0x14F-0x194, back to -6 by 0x1DE-0x222, and
0/-2 at 0x300+. So 0461 allocates less RAM in the mid-range and gains some
back above 0x222.

Instead, match on *behaviour*: normalise each function to its instruction
sequence with every symbol operand replaced by a placeholder (numeric
immediates kept), then pair functions across the two ROMs by that signature.
85 of 0461's 149 functions are an exact signature match to a 9651 function —
i.e. logically identical code — plus 60 more at >=0.75 similarity. Walking
each exactly-matched pair in lockstep then yields variable renames: where
9651 uses a hand-name and 0461 an auto-name at the same instruction position,
that is a confirmed rename.

The derived deltas reproduce the anchor-based shift map exactly, which is a
useful self-check — the two derivations are independent.

**Worked confirmations.**
- `sub_D32F` (0461) = `drive_dout1_iscv` (9651): identical xref offsets into
  the same hardware registers (+A into TIMER, +C into CPR1).
- `sub_CAC1` (0461) = `calc_ect_unk_148` (9651): identical bodies; 9651
  stores to `var_ect_unk_148` (0x148), 0461 to `unk_142` (0x142) — a -6
  delta, matching the predicted map for that region.

**Applied** to `Claude/D151804-0461.asm` (created from the parent `.ASM`,
converted CP437 -> UTF-8 as with the 9651/9661 copies): 96 renames, 649
substitutions. Names embedding their own address were adapted to the 0461
address rather than copied verbatim (`var_ect_unk_148` -> `var_ect_unk_142`,
`table_ign_blend_weight` -> `table_C12C`, and so on). Verified: assembles byte-identical
to the shipped `D151804-0461.BIN`, and `verify_assembly_match.py` reports
"Total real edit regions: 0".

**A trap worth remembering.** The first attempt failed to converge because a
fuzzy match renamed `sub_FBB7` to `adc_handler_pim`, a name 0461 already
uses — a duplicate label. Any bulk rename needs a guard against target names
that already exist in the destination file, not just against duplicates
within the rename set itself.

**Secondary throttle is 9651-only, and the ADC table proves it.** Both ROMs
have a 14-slot `table_adc_handler` in the same order with identical
`table_adc_channel` bytes, so slots correspond one-to-one. Slot 8 is
`adc_handler_trac_tps` in 9651 — it range-checks the reading, drives bit 2 of
`unk_1DC`, writes `var_trac_tps_unk_152`, and sets diagnostic flags. In 0461
the same slot is a two-instruction stub: store the raw reading to
`var_adc_unk_14C`, `jmp adc_complete`. Same physical channel, sampled and
discarded. Consistent with traction control living in a separate ECU on the
9651 car, with this ECU only monitoring the secondary throttle position.

**The chargecooler pump is not yet located.** It is not on that ADC slot
(0461 does nothing with it), and it is not one of the 0461-only functions —
`sub_DC4A` (147 insns) and `sub_DCAC` (24 insns) both turn out to be knock
scaling / error-flag code, not pump drive. Note also that the "82.7 -
Chargecooler pump/level" comment appears identically in *both* files: it is
transcribed from the generic Toyota diagnostic-code list, not evidence the
feature exists in 9651. Next place to look is inline in the main loop
(`sub_C57A`) and at the DOUT/LDOUT bit writes that differ between the ROMs.

**Left for a decision.** 10 renames were held back rather than guessed: the
`dmarx_*` / `var_flags_4E_copy_*` names that embed *both* a CPU1 and a CPU2
address (e.g. `dmarx_max_retard_23B_161`). Resolving the CPU2 half needs
0461's sibling CPU2 ROM, and the CPU1_addr = CPU2_addr + 0xDA offset should
be re-confirmed for that pair rather than assumed from the 9651/9661 pair.
Two naming conflicts also need reconciling: 0461's
`check_batch_inj_limiters` / `check_inj_limiters` are exact signature matches
for 9651's `check_limiters_active` / `check_limiters_active_2`.

---

### CPU2 (D151803-9661): unk_ variable rename pass
Went through every remaining `unk_`/`dmarx_unk_`/`dmatx_unk_` variable in
CPU2, renaming where a confirmed role exists and adding a reference note
(pointing at the confirming code, or a cross-referenced-but-equally-
unresolved CPU1 counterpart) where it doesn't:

- `unk_44`/`unk_47`/`unk_48` -> `var_flags_44`/`var_flags_47`/`var_flags_48`
  (all fully bit-documented this session - see their declarations).
- `unk_51` -> `var_rpm_deviation_51` (confirmed physical meaning:
  `|unk_EC - var_rpm_x_5p12|`, read only by external debug tooling).
- `unk_55`/`unk_56` -> `var_dma_sync_timeout_55`/`var_dma_rearm_cnt_56`
  (serial_dma_start's timeout/retry counters).
- `unk_5A` -> `var_map_temp2_5A` (an unread scratch temp alongside
  var_map_temp/var_map_temp_x in the bilinear-interpolation helper).
- `unk_103` -> `var_enrichment_unk_103` (matches its
  var_enrichment_unk_53/FE/100 decay-chain siblings).
- `unk_111`/`unk_112` -> `var_unk_111`/`var_knock_unk_112` (still
  unconfirmed purpose, but knock_unk_112 mirrors its DMA sibling
  dmatx_knock_unk_160's naming).
- `unk_11E` -> `var_tvsv_scale_base_11E` (TVSV's RPM-table base value).
- `unk_126` -> `var_asr0n_shadow_126` (confirmed role: RAM shadow of the
  real ASR0N register - see serial_dma_start's header for the unconfirmed
  bit 6/7 semantics that remain).
- `unk_E8`/`unk_EA`/`unk_EC` -> `var_rpm_smooth_e8`/`_ea`/`_ec` (three
  distinctly-smoothed RPM trackers; their exact functional differences
  remain unconfirmed, per the existing header notes).
- `dmarx_unk_DB` -> `dmarx_add_enrichment_DB`, `dmarx_unk_E1` ->
  `dmarx_dout0_duty_E1` (both already had confirmed roles from earlier
  work).
- `dmatx_unk_158/159/15A/15D` -> `dmatx_enrichment_unk_158/159/15A/15D`
  (DMA-transmitted siblings of the var_enrichment_unk_53/FE/100/103 chain).
- `dmatx_unk_169`/`dmatx_unk_16B` -> `dmatx_status1_169`/`dmatx_status2_16B`
  (confirmed role: packed status snapshots for CPU1, individual bits
  already documented in update_dmatx_status_flags).
- `dmatx_unk_16A` -> `dmatx_diag_mode_16A` (the always-dead diagnostic-
  mode selector documented in TVSV/update_odb_flags).
- `dmatx_unk_16C` -> `dmatx_ign_advance_hi_16C` (new cross-reference:
  = CPU1's `dmarx_ign_advance_hi`, written once to the fixed constant
  0xC0 at factory_selfcheck's entry).
- `dmatx_unk_162` -> `dmatx_lambda_trim_162` (confirmed cross-reference
  to CPU1's `dmarx_lambda_trim` from earlier this session, just not
  renamed at the time).

**Left unrenamed, with reference notes added:** `unk_4D`/`unk_7F`/
`dmarx_unk_D2` (genuinely unreferenced anywhere - padding/boundary
markers), `dmarx_unk_D3`/`D4`/`D5` (used only as opaque thresholds/
multipliers, no confirmed physical meaning), `dmarx_unk_4B` (single
reference, inside TVSV's dead diagnostic gate), `dmatx_unk_15F`/`167`
(cross-referenced to CPU1 where applicable, but the CPU1 side is equally
unresolved). Verified via verify_assembly_match.py after every rename -
0 real edit regions throughout (pure renames, no byte changes).

---

### CRITICAL CORRECTION: `tbs` is a destructive test-and-set, not a pure test
This session spent significant effort concluding that `var_flags_41` (CPU2)
and `var_schedule_flag_41` (CPU1, same address 0x41) were "dead" - flag
bytes that get cleared periodically by an ISR but, per an exhaustive
search (bit instructions, 16-bit spillover, indexed X/Y writes, side
effects inside every called sub-function, every `increment_counters`
range), never get set again anywhere in either ROM. That entire
conclusion was **wrong**, traced to a wrong assumption about one
instruction.

**The bug in my own reasoning:** `toshiba-8x-technical-reference.md`
described `tbs bitX, varX` as testing a bit and setting only the Z flag -
a pure, non-destructive test, like `tbbs`/`tbbc` but without the branch
built in. Under that assumption, `tbs bitN,var; bne target` looks
functionally identical to `tbbs bitN,var,target`, just longer - so a
`tbs`-tested bit that's only ever cleared really would stay clear forever
once cleared, explaining the "no setter found" mystery as dead code.

**The user pushed back twice, correctly:** first pointing out that if
`tbs`+`bne` and `tbbs` were truly equivalent, there'd be no reason for
this ROM to use the longer two-instruction form extensively instead of
the shorter combined one - a real design/space cost with no payoff under
the "pure test" reading. Then the user located and shared a real Denso 8X
test-code slide (`Test8.asm`, "Denso 8X Test Code v8.0", "BRANCH
OPERATIONS - TBS") with actual recorded test results:
```
tbs  bit0, Status06   ; test bit status AND SET IT
beq  loc_E855         ; branch if Status06.0 was clear
clrb bit0, Status06
```
"'tbs' tests the specified bit for whether it is clr or set, and *then
sets it*." "The tbbc and tbbs operands just do the test and branch, they
leave the bit untouched." Confirmed empirically: `[0041]=01` before,
`[0041]=01` after a `tbs` that read it as still-set (no branch), then
`[0041]=00`/`[0041]=01` again showing the toggle idiom's two halves.

**Corrected understanding:** `tbs` reads the bit's prior value into Z,
then unconditionally sets the bit to 1. The common `tbs`+`beq`+`clrb`
idiom is therefore a **toggle**: branch away (net 0->1) if it was clear,
otherwise explicitly clear it back (net 1->0) if it was set. Used as a
gate (`tbs`+`bne`/`beq` with no following `clrb`), it becomes a
**self-re-arming one-shot latch**: an ISR clears the bit periodically
("unlocking" it); the *first* subsequent `tbs` test both detects the
unlock (via Z) and immediately re-locks it (via the set side effect), so
further tests before the next periodic clear correctly see it as
already-handled and skip. This is exactly what the ROM's own pre-existing
comments already said ("Cleared every 16ms to trigger 16ms process",
"Unlock 64ms-gated injection schedule") - they were right all along; my
"SUSPECT DEAD" conclusion was the error, introduced by trusting a
technical-reference description that was itself never verified against
real hardware/test documentation.

**Fixed:** `toshiba-8x-technical-reference.md` and its `-part1.md`
duplicate (corrected `tbs` section, with the toggle/self-re-arming idiom
explained); `var_flags_41`'s and `var_schedule_flag_41`'s declaration
comments (both fully rewritten - the periodic gates they describe are
real, working mechanisms); `update_ign_timing_blend`'s header (CPU1); `calc_ignition_
timing`'s periodic-counters comment (CPU2); `var_flags_40.1`'s note
(CPU2); `unk_47.7`/`check_starter_running`'s one-shot-latch description
and `serial_dma_start`'s correction note (CPU2 - the original pre-
session `check_starter_running` header comment turns out to have been
correct the whole time); `var_flags_46.0`'s TVSV note (re-verified: still
inert, but because ALL its touch points - including its one `tbs` - sit
inside an already-dead diagnostic gate, not because of a missing setter).
Cross-checked every remaining plain `tbs` site in both files against this
corrected model before moving on. Verified via verify_assembly_match.py
throughout (comments/renames only, 0 real edit regions at every step).

**Lesson for future sessions:** a "no setter found anywhere, therefore
dead" conclusion about a flag that's tested via `tbs` should be treated
with suspicion by default now - re-derive from `tbs`'s destructive
semantics before concluding a gate is vestigial.

---

### Claude/ working copies converted to UTF-8 (recovering earlier silent corruption)
Both `Claude/D151803-9651.asm` and `Claude/D151803-9661.asm` were CP437-
encoded (the old IBM PC/DOS codepage IDA exported them in), not Latin-1/
CP1252 as CLAUDE.md previously claimed. Investigating a user question about
"odd bytes" (0x18/0x19 before xref type letters - already documented in
CLAUDE.md as invisible stray control bytes) turned up that they're CP437's
control-range glyphs for `↑`/`↓` (IDA's xref-direction markers) - and, more
importantly, that a **separate, much larger corruption had already silently
happened**: every CP437 byte ≥0x80 (the decorative box-drawing "SUBROUTINE"/
section-divider banners) had been mangled into the Unicode replacement
character `�` (U+FFFD) at some point during this project's prior Read/Edit
tool round-trips, which assume UTF-8. This was already baked into git HEAD
for both files (3,822 instances in the CPU2 copy, 42,235 in the CPU1 copy) -
not something this session caused, but not previously noticed either.

**Recovered, not just patched over**: the non-`Claude/` buildable siblings
(`D151803-9661.ASM`, `D151803-9651.ASM`) were never round-tripped this way
and still had clean, original CP437 bytes, including the banner lines -
which are pure unmodified IDA boilerplate never touched by any RE rename.
Reconstruction approach: line-diff the buildable file against the Claude
copy (normalizing high-byte/corruption runs to a placeholder so corrupted
and clean versions of the same boilerplate line still match as "equal"),
then for every corrupted line in the Claude copy that diffed as unchanged
boilerplate, substitute the buildable file's clean bytes (CP437-decoded).
A handful of lines (the file's own top IDA-header banner on both files, and
one mid-function section divider on CPU1 that fell outside the diff's
alignment) needed manual identification by content/context matching rather
than automatic diffing. Verified byte-for-byte: every line's ASCII-only
content matched between before/after (proving no real text was lost or
altered, only the encoding representation changed), and both files still
assemble to the identical binary as their buildable counterparts via
`verify_assembly_match.py` (0 real edit regions).

Scope: only the two actively-edited `Claude/` working copies were converted.
The rest of the repo's ~40 other `.ASM`/`.asm` files are still raw CP437 and
still carry both risks (silent corruption on any UTF-8-assuming tool
round-trip, and invisible-byte edit-tool mismatches) - not addressed this
session, left for a future decision on scope. See CLAUDE.md's updated
encoding note.

### CPU2 (D151803-9661): serial_dma_start/int_vector_0's ASR2/ASR3/TIMER3 protocol decoded
Largely closes the "serial_dma_start/int_vector_0's exact protocol/timing"
pending item - the software-side register format is now understood, even
though the electrical-level details (pins/baud rate/clock master) remain
genuinely unknown without hardware probing (see
toshiba-8x-technical-reference.md's "Known Unknowns").

- **ASR2/ASR3 register format, confirmed by arithmetic against
  `main_loop`'s own initial DMA arm** (which uses named symbols instead
  of these two functions' hard-coded constants, letting the constants be
  decoded): both are 16-bit registers written as `mode_nibble | address`.
  ASR2 (RX) = `0x9000 | var_serbus_rx` (`0x9127` on this ROM) - the raw
  incoming-frame scratch buffer `copy_serbus_rx` unpacks. ASR3 (TX) =
  `0x8000 | 0x14D` - `0x14D` is `dmatx_ve_corr_map`, the first named
  `dmatx_*` variable, meaning TX has no separate packing buffer: the
  engine streams CPU2's live `dmatx_*` variables directly.
- **`int_vector_0` confirmed = IV0** (its `IRQLL` bit-2 clear matches
  toshiba-8x-technical-reference.md's own "IV0 - Enable: set IMASKL bit
  2") - a periodic ~353Hz tick (per that doc's separate timing
  measurement), not something triggered by serial activity. So this
  whole subsystem is a fixed-rate software poll/re-arm loop, not an
  edge-triggered ISR.
- **TIMER3** is written with full 8-bit values here, which doesn't fit
  the technical reference's base-variant "Timer LSB bits [2:0]"
  description - flagged there as a probable enhanced-variant difference,
  exact bits not decoded.
- unk_55/unk_56 given confident (not fully confirmed) readings as
  timeout-retry counters; **correction**: the previous header's claim
  that this subsystem relates to unk_47.7 was wrong - unk_47.7 is never
  written anywhere in this file (by any instruction), only cleared by
  check_starter_running, which is itself an open question but unrelated
  to serial_dma_start.
- **Still open**: unk_126's bit 6/7 semantics (shadowed into the real
  ASR0N register - whether ASR0 is itself partly repurposed on this
  variant isn't established), and all electrical-level protocol details.

toshiba-8x-technical-reference.md's Appendix updated with the ASR2/ASR3/
TIMER3 findings. Verified via verify_assembly_match.py - 0 real edit
regions.

### CPU2 (D151803-9661): dmarx_unk_D6 resolved - it's CPU1's var_lambda_state
Closes the "dmarx_unk_D6's physical meaning" pending item. Discovered a
second CPU1<->CPU2 DMA offset formula in the process: for the CPU1-tx/
CPU2-rx direction (CPU1 sends, CPU2 receives - the opposite direction from
the already-documented `CPU1_addr = CPU2_addr + 0xDA`), **CPU1_addr =
CPU2_addr + 0x13B**, confirmed via four independent already-named pairs
(dmarx/dmatx tps/ect/pim/battery, all exactly 0x13B apart).

- **`dmarx_unk_D6` -> `dmarx_lambda_state`**: = CPU1's `var_lambda_state`
  (`0x211 - 0xD6 = 0x13B`, and independently confirmed by `copy_dma_tx`'s
  `st a, dmatx_unk_211` sourced directly from `ld a, var_lambda_state` -
  no intermediate computation). On CPU1, `var_lambda_state` is a
  fuel-cut/overrun-recovery timer (0x80/negative = idle, 0x66 = a decel
  fuel-cut recovery window just started, counts up by 2/call via
  `decay_lambda_state` until it overflows back to negative) - CPU2's two consumers
  (`loc_C9BC`'s `unk_E8` filter, and the fuel VE section's `dmatx_unk_162`
  clamp) both key off its sign to detect "CPU1 is currently in that
  recovery window."
- **`dmatx_unk_162`**: confirmed = CPU1's `dmarx_lambda_trim` (`0xDA`
  offset formula, exact match) - consumed by `calc_4ms_corrections`'
  lambda/AFR trim path (not itself deep-dived, CPU1 work). Matches the
  existing "likely deceleration/overrun-related" guess in the fuel VE
  section header.

CPU1's `var_lambda_state`/`decay_lambda_state`/`calc_4ms_corrections` weren't
deep-dived beyond what was needed to resolve these two DMA cross-
references - not added as new CPU1 pending work since `var_lambda_state`
already has inline comments from an earlier session (see
`divide_d_by_x+B3Cr` area, ~CA-CB in the CPU1 ASM).

Verified via `verify_assembly_match.py` - 0 real edit regions.

### CPU2 (D151803-9661): calc_params's ignition-timing/ISCV DMA group cross-referenced
Follow-up to calc_ignition_timing's write-up: calc_params's own header
flagged its 5-byte "ignition timing fallback/table values" group as "not
examined, out of scope" - checking CPU1's corresponding DMA addresses
(the `0xDA` offset formula) turned up that all five already have resolved
names on CPU1's side, so no new RE work was actually needed, just
adopting them:

- `dmatx_ign_timing_unk_165` -> **`dmatx_ign_timing_fallback2`** (= CPU1's
  `dmarx_ign_timing_fallback2`)
- `dmatx_unk_168` -> **`dmatx_iscv_duty`** (= CPU1's `dmarx_iscv_duty`,
  itself originally `dmarx_unk_242_168` - the "168" was CPU2's own address,
  already embedded in CPU1's pre-simplification name)
- `dmatx_ign_timing_fallback1`, `dmatx_ign_timing_unk_166`, `dmatx_unk_167`
  were already named; confirmed exact-address matches to CPU1's
  `dmarx_ign_timing_fallback1`/`dmarx_ign_timing_unk_166`/
  `dmarx_unk_241_167`.

All five are consumed by CPU1's `update_ign_timing_blend` (an ignition timing blend) -
traced far enough to confirm the fallback role (fallback1/fallback2
substitute for the primary values when `var_diag_errors_5.0` is set) but
`update_ign_timing_blend` itself was unexamined at the time - added to CPU1 pending work
below rather than tackled here. Full detail in fuel_calculation_system.md's
DMA cross-reference section. Verified via verify_assembly_match.py - 0 real
edit regions (renames/comments only). **Update (see "update_ign_timing_blend partially
traced" below):** this isn't knock-sensor-fault gating - `var_diag_errors_5.0`
is a repo-wide reused negate/abs() remember-bit, and `update_ign_timing_blend` uses it
purely as its own local state, unrelated to actual knock sensor faults.

### CPU2 (D151803-9661): calc_ignition_timing documented
Prose-documentation pass, closing the one still-undocumented link in the
main-loop chain (main_continue_2 -> calc_params -> calc_ignition_timing ->
TVSV -> warning-debounce - calc_params, TVSV and the warning-debounce phase
were already done).

- **`calc_ignition_timing`**: base ignition timing (`map_ignition_C12C`,
  indexed by RPM and `var_pim2_peak`) minus a `table_ignition_retard` entry
  selected by `var_input_bits.7`/PORTC.7, clamped to 0, -> `dmatx_ign_timing`.
  Also computes `var_flags_45`'s two hysteresis bits (high-RPM latch,
  cold/off-boost latch), `word_16D` (an RPM-retard table entry gated on
  both), and `var_max_retard_unk`/`dmatx_max_retard_161` - the per-condition
  knock-retard ceiling sent to CPU1 (= `dmarx_max_retard_23B_161`, already
  cross-referenced in fuel_calculation_system.md). The rest of the function
  is periodic housekeeping that happens to share this tick (counter
  increments/decay calls, three debounced PORTB output-bit drives whose
  physical purpose isn't confirmed) plus items 3/4 of the fuel VE section
  (already documented above `calc_params`).
- **`drive_DOUT0`**: a small standalone function embedded in the same
  chunk - same software-PWM-comparator idiom as `drive_DOUT2_tvsv`, but
  driving DOUT.0 from a duty cycle received directly over DMA
  (`dmarx_unk_E1`) rather than computed locally.

Verified via `verify_assembly_match.py` against the buildable `.ASM` -
0 real edit regions (comment/header additions only, no byte changes).

### Tooling: Makefiles now build via the Python assembler, not Tasm32.exe
`roms/3S-GTE/makefile.lib`'s assemble step now calls `roms/d8x_assembler/asm_d8x.py`
instead of `Tasm32.exe`. `checksum.exe`/`scramble.exe` are untouched - they still run
as separate Makefile steps on the assembler's plain `.bin` output.

Before switching, every buildable ECU source in the repo was assembled with both
`asm_d8x.py` and real `Tasm32.exe` and compared byte-for-byte. One genuine
discrepancy turned up: `asm_d8x.py`'s plain `.bin` output zero-filled the gap
*between* `.org` blocks, while `Tasm32.exe -f5F` fills that gap with `0x5F` - this
only showed up on `Jon_ST205_ECU`'s sparse-layout sources (16663/32768 bytes
differed), not on any of the other, contiguous-layout ECU sources, and matters
because `checksum.exe` sums every byte of the image. Fixed by adding a `fill_byte`
parameter to `Assembler.Assemble()` (default `0x00`, so existing callers/tests are
unaffected) and a `-p`/`--fill` CLI flag; `makefile.lib` now passes `-p 5F` to match
`Tasm32.exe`'s old behavior exactly. Re-verified all 7 distinct ECU sources
byte-identical against real `Tasm32.exe` output after the fix.

`checksum.exe`/`scramble.exe` themselves fail to launch in at least one dev
environment (`STATUS_DLL_NOT_FOUND`, independent of this change - confirmed by
running them standalone with no arguments) - this is the pre-existing DLL-load
caveat CLAUDE.md already documents for the `bin/*.exe` tools, not a regression.
Full `make.exe rom` was smoke-tested through the assemble step (succeeds, produces
a correct `.lst`) but the final checksummed `.bin` could not be produced end-to-end
in that environment for this unrelated reason.

### CPU2 (D151803-9661): shared utilities documented
Sixth slice of the prose-documentation pass - a survey of the 63
functions with clean `; End of function` markers turned up a handful of
pervasively-used utilities that had no header of their own (most of the
list is generic math-library code - divide/multiply/table-interpolate -
already adequately self-explanatory from their names and existing
per-line comments).

- **`increment_counters`**: previously entirely undocumented despite
  being called dozens of times throughout the ROM via the
  `ld d, #((counter << 8) + N); jsr increment_counters` idiom. Now
  explained: saturating-increments N consecutive byte counters starting
  at the given direct-page address.
- **`check_starter_running`**: force-resets the NE pulse counters (once
  per sustained-set period, via a one-shot latch) while `var_input_bits.2`
  is held for >= 48ms.
- **`check_startup`**: clarified that the real function is just its
  3-instruction body (the shared reset-detection check) - the long
  "FUNCTION CHUNK AT..." list above it is IDA misattributing unrelated
  code (calc_params, TVSV, factory_selfcheck, etc.) as its chunks, the
  same artifact CPU1's `divide_d_by_x` has.

### CPU2 (D151803-9661): OBD/diagnostic datastream fully documented
Fifth slice of the prose-documentation pass, closing the loop with the
vehicle speed/VF write-up above: `update_odb_flags`, `next_odb_byte`,
`table_odb`, and `output_odb_bit`.

- **`update_odb_flags`**: computes two OBD status bytes (`var_obd_flags1`
  reflects enrichment-mode bits sourced from the enrichment chain and
  `dmarx_var_flags_46`/`dmarx_unk_DB`; `var_odb_flags2` reflects
  diagnostic-condition/lambda/A-C/idle/starter bits - one bit, `.3`, is
  permanently forced set and marked "Unused (Neutral switch on A/T)" in a
  pre-existing comment, a placeholder not read from real hardware). Also
  selects the VF diagnostic signal's voltage level (0V/2.5V/5V, encoded as
  `var_vf` = 0/8/16) from lambda/O2-sensor/diagnostic-condition state, and
  is confirmed as the sole write site for `dmatx_unk_16A` (already known
  to always be 0, from earlier sessions' dead-code finding).
- **`next_odb_byte`/`table_odb`**: `next_odb_byte` walks an 11-entry table
  of ECU values (NE period, injector/ignition/ISCV OBD snapshots, RPM,
  MAP, ECT, TPS, speed, O2 reading, a fixed zero, and the two status
  bytes above) every 4ms, loading each into a shift register.
- **`output_odb_bit`**: shifts that register out one bit at a time onto
  PORTA.4, called from `int_vector_c_timer`. Confirmed **mutually
  exclusive** with `generate_vf_PORTA_4` (documented previously) on the
  same `var_input_bits.1` gate - PORTA.4 is a shared pin, outputting
  either the VF voltage/PWM signal or the OBD serial bitstream depending
  on whether a scan tool has activated the datastream. Updated
  `generate_vf_PORTA_4`'s own comment to reference this now-complete
  picture.

### CPU2 (D151803-9661): vehicle speed and VF diagnostic signal documented
Fourth slice of the prose-documentation pass.

- **`int_vector_4_kph`**: vehicle speed sensor hardware ISR (fires on ASR3
  edges). Alternates capturing rising/falling edges, applies a
  plausibility filter that rejects sub-250-tick periods as bounce/noise
  (skipped if enough time has already elapsed that any edge is obviously
  valid), and increments a saturating edge counter.
- **`update_spd`**: turns the accumulated edge count into `var_spd`, called
  every ~536ms from `iv6_4ms_process`. Uses a two-slot counter (primary +
  carry-over) as a simple period-to-period smoothing tap, then floors
  results of 2 or fewer counts to 0 (stopped) to avoid a noisy near-zero
  reading.
- **`generate_vf_PORTA_4`**: drives PORTA.4 as a TIMER-based PWM output
  encoding `var_vf` - Toyota's "VF" diagnostic-terminal duty-cycle signal,
  intended to be read with an analog voltmeter on the diagnostic
  connector. Skips while the OBD serial datastream is active so the two
  outputs don't contend for the same terminal.

Also fixed a stale cross-reference: `factory_selfcheck`'s header quoted
`loc_D2E9`'s old placeholder comment ("some kind of reset, wait for a bit
and then clear all RAM"), which was rewritten to a proper explanation in
an earlier pass. Updated the quote to match.

### CPU2 (D151803-9661): I/O input reading and DMA receive unpacking documented
Third slice of the prose-documentation pass.

- **`check_io_inputs`**: reads 8 digital inputs into a bitfield (PORTB.6/7,
  IRQLL.0 starter-running, PORTC.6, PORTA.6/7, SMRC_SIR's SIN1/SIN2), then
  applies a two-sample de-glitch filter (a bit only updates in
  `var_input_bits` if it matched in both this call and the previous one)
  before anything else trusts it. Called very frequently - confirmed
  twice-back-to-back in `loc_C88E`'s startup sequence primes both samples
  before the first real de-glitch.
- **`copy_serbus_rx`**: unpacks the raw DMA receive buffer (`var_serbus_rx`
  - the literal ASR2 destination, per `reset_vector`) into the named
  `dmarx_*` variables. Confirms `dmarx_pim2` is the first word-sized
  `dmarx_*` variable, at the same relative offset as `var_serbus_rx`'s own
  start (33 words copied verbatim, then 4 explicit single-byte copies for
  tail flag variables that don't fit the word stride:
  `dmarx_unk_4B`/`dmarx_var_flags_46`/`dmarx_flags_1`/
  `dmarx_limiter_flags`).
- **`serial_dma_start`/`int_vector_0`** - scoped out, not traced in
  detail: a low-level serial DMA hardware timing state machine
  (`ASR0N`/`ASR2`/`ASR3`/`TIMER3`, `unk_55`/`unk_56`/`unk_126`) that sets
  the "new DMA frame ready" flag `copy_serbus_rx` gates on. The overall
  role is clear; the exact protocol/timing meaning of each state value
  isn't - flagged as a dedicated future target rather than rushed here.

### CPU2 (D151803-9661): NE (crank position) interrupt architecture documented
Second slice of the prose-documentation pass, directly upstream of last
session's `calc_rpm`/`main_loop` write-up: full architecture of
`int_vector_e_asr2`/`int_vector_c_timer`/`int_vector_6_sw_int`/
`iv6_ne_process`, resolving two pre-existing "; ????" comments in the
process.

- **`int_vector_e_asr2`** (hardware ISR on every NE edge): updates
  `var_ne_count` (position 0-5/cylinder 0-3 counter) - confirmed the
  G1/G2 sync markers (`0x35`/`0x15`) are identical to CPU1's own
  documented convention (`docs/ignition_system.md`), even though CPU2
  reads G1/G2 via direct `PORTD_ASRIN` pins rather than CPU1's
  timer-capture scheme.
- **`int_vector_c_timer`** (periodic hardware TIMER ISR): drives
  `output_odb_bit` periodically, otherwise just a trigger source.
- **`int_vector_6_sw_int`** - the key piece: this is the actual "IV6"
  vector (external interrupt 6), triggered in software by both ISRs
  above. Runs `iv6_ne_process` and `iv6_4ms_process` as siblings sharing
  one interrupt priority level, each gated by its own test-and-set
  "already ran" flag (`var_flags_40.4`/`.3`) so bursts of re-triggers
  collapse into at most one pass of each per invocation.
- **`iv6_ne_process`** (the NE background processor): samples the ASR2
  hardware edge-counter into a 3-slot `var_ne_table` ring buffer at even
  crank positions only, sums it into `var_ne_sum` (which `calc_rpm`
  converts to RPM). **Resolved the "; ????" comments**: `var_cnt8ms_AF`
  doubles as a "no valid previous NE data" flag (via `xch`, which both
  stores this call's flag and retrieves the previous call's), used to
  fall back to the same `0x5500` sentinel `init_ne_counters` uses when
  there's no valid prior ASR2 sample to diff a period against.

Full detail in the header comments above `int_vector_e_asr2` and
`iv6_ne_process` in the ASM.

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
(`var_pim_tps_est`, `var_pim_est_fast`/`135`, `var_nv_trim_unk_98`, `unk_1CA`) are
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
  diagnostic check run (loc_DDB8-DE7B, reached via update_diag_obd, itself called
  from a *different, later* point in the main loop - the same short second
  var_trim_state-alias instance that calls update_lambda_stft). Added a matching
  `unk_1CF_alias` .equ (same technique as var_trim_state_alias) and applied
  it to both confirmed windows.
- Resolved two previously-flagged "not deep-dived" helpers: `check_cnt_187_window`
  (resets unk_187, sets var_flags_4F.7 if outside [3,0x131)) and
  `inc_cnt_187` (saturating increment of unk_187) - a simple counter/error-flag
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
(var_trim_state -> var_flags_4E -> jsr update_lambda_stft -> var_flags_4E ->
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
and update_lambda_stft's full body (through locret_DB74, including clear_trim_state_bit2/
clear_trim_state_bit0 - discovered this session to be a SEPARATE short-lived instance of
the same trick, called from much later in the main loop, not part of
D931's direct continuation). Also found var_cnt_6A's consumer while
tracing update_lambda_stft: loc_DB34 gates trim_state.5 on "var_cnt_6A >= 3 ticks".
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
- Calls apply_enrich_and_trims (fuel enrichment scaling, confirmed), calc_dmatx_pim, validate_nv_trim_pim,
  validate_nv_trim_o2 (NV trim validation, see D1DD below) - only validate_nv_trim_o2 was
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
- Periodic counter increments plus calc_rpm_delta, decay_lambda_state (lambda_state
  decay), ramp_misfire_correction, inc_cnt_187 (not deep-dived).
- A **second closed-loop lambda trim system** (`closed_loop_control`
  label), distinct from the RPM/MAP-zone `nv_afr_trim_base` system in
  calc_4ms_corrections' chunk CE6C. Gated on ECT 83-104C, off-idle,
  RPM<3200, battery>=11.4V, and `var_trim_state==4`. Accumulates O2
  sensor polarity into `var_o2_vote_accum` over 17 samples
  (`var_o2_vote_cnt`), then nudges `var_nv_trim_unk_96` by +/-1 via a
  majority-style threshold. Not renamed - didn't confirm what
  specifically distinguishes this from the zone-based AFR trim (e.g.
  "cruise" vs "part-throttle"), worth a follow-up.
- `validate_nv_trim_o2`: validates `var_nv_trim_unk_96` against `nv_96_limits` and
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
- clear_variables: byte-fill var_flags_40..unk_7F, word-fill var_diag_errors_4..dmarx_ign_advance_lo
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
being past the cranking/stall band, an idle-debounce latch (var_flags_44.0), and
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

### CPU1 (D151803-9651): flags-variable and unk_ variable pass

Brought CPU1 up to the standard CPU2 reached in the previous session (the
same two-part brief: every "flags" variable documented bit by bit, every
`unk_` variable either properly named or given a reference to where it is
set and read). Assembly equivalence re-verified after every edit -
`verify_assembly_match.py` reported "Total real edit regions: 0" throughout.

**Flags variables - all bits now documented.** Filled in every blank in the
pre-existing address-keyed bit templates and added templates where none
existed: `var_flags_40` (was `unk_40`), `var_schedule_flag_41`,
`var_flags_42`, `var_limiter_flags`, `var_flags_44` (was `unk_44`),
`var_ignition_flags`, `var_flags_46`, `var_flags_47` (was `unk_47`),
`var_diag_errors_5`, `var_io_input1`/`var_io_input2`,
`var_error_flags1`/`var_error_flags2`, `var_flags_4D`, `var_flags_4E`,
`var_flags_4F`, plus `var_flags_1DC` (was `unk_1DC` - a flags byte that
earlier bit-op sweeps missed entirely because it is manipulated with
whole-byte AND/OR masks rather than `setb`/`clrb`).

**Renames (18).** `unk_40`->`var_flags_40`, `unk_44`->`var_flags_44`,
`unk_47`->`var_flags_47`, `unk_1DC`->`var_flags_1DC`,
`unk_1BD`->`var_pw_loop_mode`, `unk_302`->`var_nv_trac_tps`
(+`nv_302_limits`->`nv_trac_tps_limits`), `unk_101`->`var_tps_closed_ref`,
`unk_EF`->`var_tps_closed_cnt`, `unk_FA`->`var_pim_baseline`,
`unk_11D`->`var_tps_delta_prev`, `unk_11E`->`var_tps_delta_rate`,
`unk_A6`->`var_limiter_ign_ramp`, `dmatx_unk_206`->`dmatx_inj_pw_inj1`,
`dmatx_unk_211`->`dmatx_lambda_state`, `dmatx_unk_21C`->`dmatx_pw_loop_mode`,
`dmarx_unk_243`->`dmarx_status1_169`, `dmarx_unk_245`->`dmarx_status2_16B`,
and the three ROM constant tables `unk_C2EE/C2F2/C2F7`->`table_unk_*`.

The two `dmarx_status*` names come from CPU2's side: that ROM's
`update_dmatx_status_flags` already documents both bytes as packed status
snapshots and gives the exact bit-to-source mapping, so the CPU1 receive-side
names now match across the DMA boundary.

**Reference notes (43).** Every remaining `unk_` declaration got a factual
"written by X, read by Y" note generated from the actual instruction stream
(not from the IDA xref comments), so the fallback half of the brief is
satisfied for all of them. Deliberately left named `unk_` per CLAUDE.md
rather than guessed at.

**Findings worth flagging (all corrections or dead code, not just naming):**
- **`var_schedule_flag_41`'s entire bit table was mislabeled `40.X`** - a
  copy/paste from the `unk_40` declaration above it during last session's
  `tbs` correction. The content was right, the address prefix was wrong.
  Fixed to `41.X`.
- **`var_flags_40.6` looks genuinely dead**: no `setb` exists anywhere in the
  file, only `clrb` plus non-destructive `tbbs`/`tbbc` reads. It starts clear
  and is only ever re-cleared, so every "jump if set" site never jumps -
  including `check_set_overrun_flag`'s own "init guard active: skip" gate.
- **`unk_14A` is permanently zero**: exactly one reference in the whole file
  (`sub a, unk_14A` in the rev-limiter hysteresis check) and no write site at
  all, which makes the subtract a no-op. Reads like a tunable that was
  disabled by zeroing rather than a live variable.
- **`unk_E0` is completely unreferenced** - a genuinely unused RAM byte.
- Eight more are write-only in this file (written, never read here) - either
  consumed by CPU2 over the DMA buffer or vestigial; each is marked as such.
- **`var_flags_4D.2` was documented wrongly** by an earlier pass as
  "acceleration enrichment... opposite direction" to `var_flags_44.2`. It is
  actually the *second* derivative of throttle position (`var_tps_delta_rate`)
  in the *same* closing direction - the pair distinguishes "closing fast"
  from "closing ever faster". Corrected.
- **`injector_cold_start`'s `var_flags_42.4` gate was mis-described** as an
  "ADC phase 1 complete" check (a guess predating the `tbs` correction). It
  is the function's own private one-shot self-lock; the real ADC-phase flag
  is bit1. Corrected at both the declaration and the use site.
- **`var_flags_44.7`**: because its `tbs` self-lock fires on the very first
  4ms tick after STA goes high, the NE-counter resync guarded behind it can
  only run on that same tick - when `var_4ms_cnt_sta` is still 0 and the 0x0C
  threshold therefore always fails. As written that resync path reads as
  unreachable. Flagged rather than asserted; ruling it out for certain needs
  confirmation that nothing else advances `var_4ms_cnt_sta` while bit7 is held.

**Still open on CPU1** (unchanged by this pass): the pending-work list below.
This pass was breadth-first over variables, so `calc_dmatx_pim`, `loc_E112`/`E363`,
`update_ign_timing_blend`'s middle blend and the second lambda-trim system are all still
untouched - several of the reference notes above point into them.

### CPU1 (D151803-9651): function Inputs/Outputs pass

Second half of the same brief - "each callable function should have inputs
and outputs section, if there are values passed in and out via registers,
otherwise it should document which variables are read and written to". Now
**145 of 150 call targets** carry one (the other 5 are two-instruction branch
stubs whose one-line comment already says everything). Purely additive: zero
deleted lines, and `verify_assembly_match.py` stayed at 0 real edit regions
throughout.

**Method.** The variable footprints were *generated from the instruction
stream*, not transcribed by hand or taken from the IDA xref comments - a
small extractor walks each function body (following its chunks), classifies
each operand as read or written per opcode form, and emits sorted
`Reads:`/`Writes:`/`Calls:` lists. Validated by running it against
`update_tps_closed_ref`, whose footprint had already been derived by hand earlier in the
session: exact match. This matters because the IDA `DATA XREF` headers are
incomplete - they show a couple of representative sites and then "...".

**Hand-written headers** for the functions where purpose needed prose rather
than a footprint: `iv6_ne_process` (the ~212-instruction crank-synchronous
process, and why it is split off from the hardware NE vector via the IRQLL.1
software interrupt), `knock_processing` (and how it differs from
`knock_mcu_update` next to it), `factory_self_test`, `check_io_inputs`, `copy_dma_tx`,
`clear_nv_ram`, plus register-level Inputs/Outputs for `deglitch_io_input`,
`write_rB_nv_ram`, `increment_counters`, `add_d_base_offset`,
`scale_d_by_a_frac` and both divide cascades. `calc_iscv` already had an
excellent SECTION 1-6 breakdown and just gained the footprint.

**Things established while writing these:**
- **`factory_self_test` is a factory/end-of-line self-test**, and its entry interlock
  (diagnostic mode AND starter AND two PORTA pins AND TRAC TPS > 0x9A) is why
  it normally returns immediately. It sets `var_flags_40.0` and then scribbles
  a walking 0..255 pattern over all of RAM (0x40-0x300) to test the chip -
  **which is what `var_flags_40.0` is for**, and explains why that one bit is
  read in dozens of otherwise unrelated places: they are checking "is my state
  currently garbage?". That closes the loop on the widest-read bit in the ROM.
- **`deglitch_io_input` is a two-sample debounce**, derived bit by bit:
  `[X] = ((N XOR C) AND T) + (N AND C)`, so a bit only moves once two
  consecutive samples agree. The `add` works as an OR because the terms are
  disjoint per bit. This is also why `var_io_input1`/`2` have no direct store
  site - they are written here through `X`.
- **`write_rB_nv_ram` maintains a per-entry check byte** at `[X+1]`
  accumulating `(old - new)`, written together with the value in one 16-bit
  store so the pair cannot be torn by an interrupt. Same value/complement
  idea as `clear_nv_ram`'s 0x6699 seed, and it is what the startup validity
  check tests before deciding to wipe NV RAM.
- **The `divide_rD_N_saturate` family does not actually saturate** - it
  performs exactly the same shifts as the plain family. The separate labels
  are a readability convention marking call sites that follow a saturating
  multiply. Noted in the header so nobody infers behavior from the name.
- `add_d_base_offset` and `inj_overrun_end_2` are further instances of the
  fall-through / shared-tail code-reuse pattern already documented for
  `set_knock_sensor_err_flag`.

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

## Naming status (both MR2 ROMs)

**Every callable in both ROMs is now properly named.** Measured, not asserted:

| ROM | call targets | still `sub_`/`loc_` |
|---|---|---|
| CPU1 D151803-9651 | 150 | **0** |
| CPU2 D151803-9661 | 56 | **0** |

The last 22 on CPU1 were cleared in one pass. Most were small helpers whose
behaviour was obvious once read - `sub_CB00` -> `decay_lambda_state`,
`sub_C91A` -> `calc_rpm_delta`, `sub_E843` -> `scale_by_nv_trim_o2`,
`sub_E435` -> `update_crank_cnt` (plus `unk_9F` -> `var_crank_cnt`),
`sub_E454` -> `apply_enrich_and_trims`, and so on.

Two are named but only PARTIALLY traced, and their headers say so plainly
rather than implying more than was established:
- `update_cyl_rpm_dev` (was `loc_EDCB`) - the per-cylinder crank-speed
  deviation measurement at its head is confirmed; the tail that folds that
  roughness figure into an ignition correction is not.
- `update_diag_obd` (was `loc_DD69`) - the ISC self-check at its head is
  confirmed; the rest is characterised by variable footprint only.

Note on scope: the ~1,400 remaining `loc_XXXX` labels are ordinary
in-function BRANCH targets, not callables. Even the gold-standard
`knock_mcu_update.ASM` has those, and renaming them is not a goal - the
metric that matters is call targets.

Still named `unk_`: 52 on CPU1, 13 on CPU2 (down from 60 after a pass over
the most-referenced ones). Every one carries
either a full explanation or a factual "written by X, read by Y" note; they
are unnamed because their purpose is genuinely unestablished, not because
nobody looked.

### `unk_` pass: the eight most-referenced on CPU1

| was | now | basis |
|---|---|---|
| `unk_127` | `var_ign_blend_out` | update_ign_timing_blend's signed output; feeds PW stage 3 and an ISC gate |
| `unk_129` | `var_ign_blend_accum` | the signed accumulator in that blend; its sign selects table_rpm_C168/C172 |
| `unk_AA` | `var_fuelcut_recovery_cnt` | 0..2 counter; reset while fuel cut active, gates a +0x01F4 bump for ~2 ticks after it releases |
| `unk_187` | `var_cnt_187` | 16-bit saturating counter with an accept-window of [3, 0x131); what it counts is still unknown |
| `unk_1DD` | `var_asr0n_shadow_1DD` | **CPU1's ASR0N write-shadow** - see below |
| `unk_1D8` | `var_flags_4F_copy2` | save/restore slot for var_flags_4F, parallel to var_flags_4E_copy2 |
| `unk_1CA` | `var_inj_pw_unk_1CA` | intermediate on the injector-PW path; domain certain, quantity not |
| `unk_E5` | `var_cnt_sta_active` | starter-engaged tick counter; >0x1F (~124ms) debounces the STA fault |

**`var_asr0n_shadow_1DD` is the notable one.** CPU1 maintains an ASR0N write
shadow using the identical idiom to CPU2's `var_asr0n_shadow_126`, for the
identical reason: ASR0 write configures DMA while ASR0 read returns the
latched I/O-transition timer value, so the register cannot be
read-modify-written. Nobody had connected this CPU1 variable to that
mechanism - it was found by recognising the shadow idiom once the hardware
asymmetry was known. Worth remembering that a fact learned about one CPU is
worth re-scanning the other for.

`var_cnt_sta_active` is also a reminder of the `increment_counters` blind
spot: it appears only ever CLEARED in this file, because its increments come
from the `COUNTER_ARG(var_cnt_E1, 7)` range write.

---

## Pending work (next targets)

Split by which CPU/ROM each item belongs to - CPU1 (D151803-9651) and
CPU2 (D151803-9661) are physically separate chips with separate address
spaces; addresses/chunk names below are only meaningful within their own
ROM.

### CPU1 (D151803-9651)

**Functions renamed but not commented:** RESOLVED - `calc_ign_timing_min`
and `check_limiters_active`/`_2` both have header blocks as of the
Inputs/Outputs pass above, along with 143 other call targets. Note this is
header/footprint coverage, not a deep trace: `calc_ign_timing_min`'s header
records what it reads and writes, but the meaning of its `var_flags_4E.3`
entry gate is still unestablished (see that bit's declaration comment).

**Not yet started:**
- **RESOLVED: sub_E551 is `calc_dmatx_pim`** - manifold-pressure transient
  compensation, NOT the "knock/boost limiting calculation" this entry
  previously guessed. Full writeup now lives in the function's own header
  and in fuel_calculation_system.md; summary here.

  The tell is loc_E627, its single exit: `st d, dmatx_pim`. Everything
  above exists to decide what value CPU2 gets for fuel calculation. The
  `set_knock_sensor_err_flag`/`check_knock_sensor_err_flag` calls that
  drove the knock hypothesis are just the generic abs()/restore-sign
  primitive (see the correction recorded for those functions) - they carry
  no knock meaning, and the old `var_unk_knk_133`/`135` names came from
  exactly that mistake.

  **Mechanism.** A MAP sensor lags the real manifold event, so fuelling
  from `var_pim2` alone runs lean on tip-in and rich on tip-out. The ECU
  therefore builds an independent throttle-derived pressure estimate and
  filters it twice at different rates, using the divergence between the
  filters as a "how fast is load changing" signal - a lead/lag pair:

      get_tps_unk        var_tps + var_unk_tps_143*0x40
      get_tps_load_div8           the above >> 3
      var_pim_tps_est    * var_pim_trim_scale * 2   (in calc_dmatx_pim)
      var_pim_est_fast   tracks var_pim_tps_est     (update_pim_est_fast)
      var_pim_est_slow   tracks fast at 1/4 rate    (update_pim_est_slow)

  `var_pim_trim_scale` is the learned PIM/barometric NV trim
  (`var_nv_trim_unk_98`, via adc_handler_pim) normalised to a multiplier -
  that is what puts the throttle estimate into real PIM units. Both
  filters force-load their input while `var_flags_46.0` is set, so they
  start converged at cranking instead of injecting a false transient.

  calc_dmatx_pim then derives a step count (clamped to 14) from injector
  pulse width, injector trim and RPM, runs the filter that many times - so
  it converges faster the bigger the load event - takes
  (result - var_pim_est_slow), scales it against var_pim2 above a 0x61
  threshold, and adds it to var_pim2 with saturation. It bails and passes
  raw var_pim2 straight through during the factory self-test
  (var_flags_40.0), while cranking (var_flags_46.0), and notably on a hard
  throttle CLOSE (var_flags_47.7).

  **The link to fuel trim** (and to Jon's short-term/long-term note above):
  it publishes `var_pim_trans_fast` = sign(fast - slow), and
  `closed_loop_control` refuses to run trim learning while that reads more
  negative than -2 (`cmp a,#0FEh` / `blta`). That is the classic "freeze
  fuel trim through a transient" rule - trims only adapt once the two
  filters reconverge. Worth carrying into the trim-cluster session.

  **Renamed as a result:** sub_E551->calc_dmatx_pim, knock_unk_E79E->
  update_pim_est_fast, some_knock_averaging_calc->update_pim_est_slow,
  var_unk_knk_133/135->var_pim_est_fast/slow, unk_131->var_pim_tps_est,
  unk_144->var_pim_trim_scale, unk_146/147->var_pim_trans_est/fast.

  **Still open here:** calc_transient_terms's two maps (map_transient_mag, map_transient_gain) are indexed
  by var_rpm_x_5p12 but their units are not established, and
  var_unk_tps_143's own producer is untraced. **Related and still not
  deep-dived:** validate_nv_trim_pim (called from chunk C9DA alongside this function).
- **RESOLVED: `loc_E112` is a one-instruction trampoline**, and `E363` is
  the 64ms dispatch slot. This entry was larger in the imagination than in
  the ROM.

  `loc_E112` is literally `jmp bg_64ms_dispatch` - the DC77/DD38/DD59
  diagnostic phase ends by jumping here and this forwards straight on. There
  is no body to trace.

  `loc_E363` -> **`bg_64ms_dispatch`**, gated by `var_schedule_flag_41.7`'s
  `tbs` one-shot (cleared by iv6_4ms_process's 64ms sub-slot, re-locked by
  the test itself). Structure:
  - `loc_E36A` - counter maintenance: bumps the 0xE1-0xE7 block, then
    `var_64ms_prescale` (was `unk_E8`); when that wraps to 1, i.e. once per
    256 slots (~16.4s), it also advances `var_cnt_E9`. **That two-stage
    prescaler is what makes `var_cnt_E9`'s thresholds in the ISCV code
    (0x17/0x26/0x99) mean roughly 6, 10 and 42 minutes rather than
    seconds** - previously those constants had no explanation.
  - `loc_E37F` - the 64ms work list: `update_crank_cnt`, `calc_ect_unk_148`,
    `calc_ect_unk_160`, `calc_ect_iscv`, `calc_ect_unk_194`, `update_lambda_stft`,
    `update_diag_obd`, `factory_self_test`. The `var_flags_4E` restore just before
    `calc_ect_unk_194` is "the restore point around E37F" this entry referred to:
    it CLOSES the long `var_trim_state` alias opened in `calc_inj_pw_base`,
    and `update_lambda_stft` is then wrapped in its own short second alias
    instance.
  - `loc_E3A6` - the tail, run on EVERY pass regardless of the gate:
    `apply_enrich_and_trims` then `calc_dmatx_pim`. So fuelling is per-tick while the
    ECT/ISCV/trim housekeeping above it is 64ms.

  Renames: `loc_E363` -> `bg_64ms_dispatch`, `unk_E8` -> `var_64ms_prescale`.
- **RESOLVED: which trim is short-term and which is long-term.** Jon
  confirmed the ECU runs both; the code says exactly where each lives.

  The decisive site is `apply_enrich_and_trims` at `loc_E47B`, which assembles the whole
  fuel correction as a single multiplier applied to injector pulse width:

      multiplier = 0x0100 (unity)
                 + read_nv_afr_trim(load)   <- LONG-term
                 + var_lambda_integrator    <- SHORT-term

  - **STFT = `var_lambda_integrator`.** Fast O2 feedback integrator, plain
    RAM, never written to NV, forced back to its 0x8000 neutral on reset
    events (loc_D04F). Swings continuously as the sensor crosses.
  - **LTFT = the `nv_afr_trim_base` table.** 12 bytes in battery-backed NV,
    indexed by `var_pim2` (manifold pressure = load) with interpolation
    between cells via `read_nv_afr_trim`. Returns the first/idle cell
    directly when the throttle is closed (`var_flags_46.2`), and a neutral
    0x80 when `var_flags_42.0` says trims are invalid. Validated at startup
    against `nv_afr_trim_top`/`_end`; failure wipes all NV via
    `clear_nv_ram`.

  So the earlier "how many trim systems are there" confusion was really
  just STFT and LTFT summed into one correction, exactly as expected.

  **But there IS a genuine third mechanism**, and it is not part of that
  multiplier: `var_nv_trim_unk_96`, learned by `closed_loop_control`. It
  votes on O2 polarity into `var_o2_vote_accum` (+/-1 per sample from a
  0x80 neutral) across a 17-sample window counted by `var_o2_vote_cnt`
  (samples 0-6 discarded as settling), then applies a majority verdict with
  a 0x7D-0x83 deadband to nudge the stored value by ONE step. Its gating is
  far tighter than the LTFT table's: ECT 82.9-103.8C, off-idle, RPM < 3200,
  battery >= 11.4V, no transient (`var_pim_trans_fast`), no CPU2
  fuel-trim/idle-enrich request, and `var_trim_state == 4`. That reads as a
  slow cruise-only global correction.

  It is stored as a value/complement pair (the two halves stepped in
  opposite directions by `dec a`/`inc b`), the same integrity scheme
  `write_rB_nv_ram` uses, and `validate_nv_trim_o2` wipes all NV RAM if it fails
  validation against `nv_96_limits`.

  **RESOLVED - the third trim is a FUEL correction, spent on CPU2.** Its
  consumers are `scale_by_nv_trim_o2` on CPU1 and, more importantly, CPU2 via
  `copy_dma_tx`'s `dmatx_nv_trim_o2`. CPU2 receives it as
  `dmarx_nv_trim_o2` and uses it in `check_startup`'s `loc_CB6D` chunk as
  a multiplier on an ECT-indexed `table_C393` lookup; the product becomes
  `var_enrichment_unk_100`, the warm-up enrichment that
  `decay_enrichment_unk_100` then bleeds away (gated on `dmarx_ect`
  0x0F-0xC4 and `dmarx_var_flags_46.0`).

  So CPU1 learns this correction slowly from the O2 sensor but mostly
  SPENDS it through CPU2's enrichment path - which is exactly why it never
  appears in CPU1's own `apply_enrich_and_trims` STFT+LTFT multiplier.

  **CORRECTION - DMA offset direction.** Finding this exposed an error
  made earlier in this same session: the `CPU1 = CPU2 + 0xDA` formula
  applies to the CPU2->CPU1 direction only. **The CPU1->CPU2 (dmatx)
  direction uses `CPU1 = CPU2 + 0x13B`.** Using 0xDA on a dmatx address
  lands inside CPU2's `var_serbus_rx` buffer, which is what flagged the
  mistake. The correct offset is confirmed by the many name pairs that
  already matched independently across the buffer: `dmatx_pim2`/
  `dmarx_pim2`, `dmatx_tps`/`dmarx_tps`, `dmatx_ect`/`dmarx_ect`,
  `dmatx_lambda_state`/`dmarx_lambda_state`,
  `dmatx_knock_retard_info`/`dmarx_knock_info`. Both offsets are now
  stated explicitly wherever they are used.

  **Bonus resolutions from walking that mapping** - four CPU2 receive
  slots that were marked "physical meaning not confirmed" are now named
  from their CPU1 sources:
  - `dmarx_unk_D5` -> `dmarx_nv_trim_o2` (this trim)
  - `dmarx_unk_D3` -> `dmarx_cnt_startup` (= CPU1 `var_cnt_startup`).
    This explains its two readers, which had been unexplained: the
    `cmp #5Ch` in `decay_enrichment_unk_100` and the `cmp #3Dh` in the
    warning-debounce phase are both "has the engine been running long
    enough" gates.
  - `dmarx_word_CB` -> `dmarx_inj_pw_inj1` (= CPU1 `var_inj_pw_inj1`)
  - `dmarx_unk_D2` -> `dmarx_nv_trim_pim` (= CPU1's PIM/barometric NV
    trim `var_nv_trim_unk_98`). Notable: it is still referenced NOWHERE in
    CPU2 - CPU1 sends its barometric trim and CPU2 ignores it entirely.
    Either vestigial or reserved for a variant; worth knowing before
    assuming the DMA layout is fully live.
  - `dmarx_unk_D4` left named unk_ but cross-referenced: it is CPU1's
    `var_cnt_EA`, whose meaning is unestablished on that side too, so
    resolving either resolves both.

  **Follow-up on the dead `dmarx_nv_trim_pim` field - and a correction.**
  Chasing why CPU2 ignores CPU1's barometric trim turned up something more
  generally useful: **CPU2's `dmarx_*` variables are not written by the DMA
  hardware at all.** The link fills `var_serbus_rx`, and `copy_serbus_rx`
  then bulk-copies that buffer into the named variables. So a symbol-level
  search for "what writes `dmarx_X`" finds nothing for every field in the
  block - the write is always that one loop.

  That means the earlier note was overstated: `dmarx_nv_trim_pim` (0x0D2)
  *is* written, because it sits inside the 0x0C5-0x0E2 span the loop copies.
  What it lacks is a READER - nothing consumes it, and it is not among the
  addresses `table_odb` serializes onto the diagnostic stream. The
  conclusion holds (CPU1 sends its barometric trim, CPU2 does nothing with
  it) but the reason is "no reader", not "untouched memory".

  This is the third time in this effort that a search blind to one write
  mechanism produced a wrong "dead" claim - after `tbs`'s set side effect
  and `increment_counters`' range writes. The pattern is worth naming: on
  this target, **writes frequently happen through a mechanism that does not
  mention the symbol**, so "no writer found" is weak evidence and needs a
  positive explanation before it becomes a conclusion.

  **Also corrected while there:** `copy_serbus_rx`'s own comment claimed the
  loop copies "33 words". It copies **15 words (30 bytes)** - it runs while
  X < `var_spd_edge_count`, i.e. 0x0C5..0x0E2. The implied source span,
  `var_serbus_rx+0x00..+0x1D`, is exactly contiguous with the tail byte
  copies at +0x1E, which is an independent check that 15 is right.

  **RESOLVED - the dead field is vestigial, not variant-reserved.** I had
  wrongly claimed the repo held no second CPU2 to test against; Jon
  corrected that. **The ROM pairs are CPU1/CPU2 by consecutive part
  number:**

  | Vehicle | CPU1 | CPU2 |
  |---|---|---|
  | JDM Gen3 SW20 | D151803-9651 | D151803-9661 |
  | JDM ST205 | D151804-0461 | **D151804-0471** |
  | UK ST205 | D151804-0481 | **D151804-0491** |

  `roms.txt` lists all five but does not say which side each is, which is
  what misled me - worth stating there if that file is ever touched.

  Checking D151804-0471 (the ST205's CPU2) settles it. Its `sub_D754` is the
  structural twin of `copy_serbus_rx`: the same 15-word copy, the same four
  tail bytes, read from the **same source offsets**
  (+0x1E/+0x20/+0x21/+0x22). So the wire protocol is byte-for-byte
  identical across the two generations - a genuinely useful fact for the
  0461 port work. Its receive block simply sits 2 bytes higher (base 0x0C7
  vs 0x0C5), confirmed independently by `dmarx_tha`, which is *already
  named* in that ROM at 0x0D1 against 9661's 0x0CF.

  The barometric-trim slot therefore maps to **0x0D4 in 0471** - and that
  address has no label and no xref there either. It is a bare, unnamed
  `.block 1` sitting between `unk_D3` and `unk_D5`, which in an IDA export
  means nothing references it.

  So **both CPU2s in the family ignore the slot**: CPU1 populates a protocol
  field that no CPU2 reads. Vestigial, not reserved.

### Applying the protocol identity: D151804-0461's DMA block named

The layout match is worth cashing in, so the whole `dmatx_*` block on the
ST205's CPU1 is now named from 9651's. **17 fields renamed**, taking 0461
from 10 named DMA fields to 27.

**Confidence, by field.** Eleven were confirmed directly from 0461's OWN
`copy_dma_tx` code - the source variable matches 9651's, so these are not
address arithmetic: e.g. `unk_200 <- var_inj_pw_inj1` (`dmatx_inj_pw_inj1`),
`unk_207 <- nv_unk_trim_98` with the same 0x50 fallback (`dmatx_nv_trim_pim`),
`unk_20A <- nv_unk_trim_96` with the 0x00 fallback (`dmatx_nv_trim_o2`).

The other six are written outside `copy_dma_tx`, and were confirmed by xref
shape instead - same enclosing function, near-identical offset:
`unk_210` read at `sub_E996+49F` against 9651's `calc_4ms_corrections+49E`;
`unk_212` written at `iv6_ne_process+99` against 9651's `+9A`. Weaker than a
source match but still specific.

Underpinning all of it: the 10 DMA fields 0461 *already* had named land
exactly where the offset mapping predicts (delta -6), 10 for 10 across the
full +0x00..+0x22 span. That is the real evidence the mapping is sound.

**CORRECTION it forced - the protocol layout is shared, the CONTENT is not.**
The previous entry called the protocol "byte-for-byte identical". Too strong.
0461 writes `var_tps_delta` at +0x1D; 9651 leaves that byte untouched. The
9651 declaration had absorbed it into a 2-byte `dmatx_pw_loop_mode` with a
comment guessing it was "padding to match CPU2's struct layout" - wrong on
both counts. It is a real protocol field, and two independent sources say so:
0461 populates it, and the SAMC21 gateway firmware's DMA struct (written
against the 0461 layout) names `dmatx_tps_delta` in exactly that position.
9651's declaration is now split, with the byte named and marked
not-written-by-this-ROM.

So: same offsets, different occupancy. Anything decoding this link
generically must not assume every slot is live on every ROM.

  CPU1-side renames: `dmatx_trim_unk_20D` -> `dmatx_nv_trim_pim`,
  `dmatx_trim_unk_210` -> `dmatx_nv_trim_o2`.

  Renames: `unk_6B`->`var_o2_vote_cnt`,
  `var_lambda_count_unk_6C`->`var_o2_vote_accum`. `var_lambda_integrator`
  and `nv_afr_trim_base` keep their established names (they are already
  meaningful and widely cross-referenced) but their declarations now state
  the STFT/LTFT identity outright.
- **RESOLVED: `loc_DA63` is `update_lambda_stft`** - the O2 closed-loop
  controller, i.e. the thing that actually drives the short-term fuel trim.
  It was NOT "yet another distinct lambda-trim mechanism" as this entry
  guessed; it is the controller for the STFT already identified above, so
  the fuel-trim picture is now closed:

  | Piece | Role |
  |---|---|
  | `update_lambda_stft` | the O2 control law - moves `var_lambda_integrator` |
  | `nv_afr_trim` table | LTFT, learned per load cell, NV-backed |
  | `apply_enrich_and_trims` @ `loc_E47B` | sums STFT + LTFT into one multiplier |
  | `closed_loop_control` | learns the separate cruise-only NV trim spent by CPU2 |

  It implements a textbook **jump-and-ramp** (proportional + integral) law:
  - **Jump** (`loc_DABF`): `var_lambda_avg >= 0xB3` (rich) adds `0x07AE` to
    `unk_1C4`, `<= 0x4D` (lean) subtracts it. `0x4E`-`0xB2` is a deadband
    that simply exits - that is what stops the loop chattering at stoich.
  - **Ramp** (`loc_DB41`): +/-`0x0010` per tick following
    `var_adc_lambda`'s sign, each gated on the integrator not already
    being at its `0x85`/`0x76` rail.

  Alongside the jump it steps `var_lambda_integrator` by `0x0F5C` (clamped
  `0x1A00`/`0xE600`) and `var_lambda_avg` by `0x0F` (clamped `0x1A`/`0xE6`).
  Those clamp pairs are the same numbers scaled by 256, so `var_lambda_avg`
  is deliberately kept tracking the integrator's high byte rather than being
  an independent quantity - worth knowing before treating them as separate.

  **CORRECTION - `increment_counters` writes are invisible to symbol
  searches.** Tracing this function's `unk_E4` gate exposed a systematic
  flaw in the automated reference notes added earlier this session: that
  helper increments a whole ADDRESS RANGE from a packed `COUNTER_ARG`
  argument, so none of its writes appear in a per-symbol grep. Five
  variables were mis-described as a result and are now fixed:
  - `unk_E0` was called "UNREFERENCED - a genuinely unused RAM byte". Wrong:
    it is the last byte of `COUNTER_ARG(var_cnt_CD, 0x14)` and is
    incremented every tick. Renamed `var_cnt_E0`. (Still effectively dead,
    but as spare counter capacity, not untouched memory.)
  - `unk_E3` was called "write-only". Wrong for the same reason - its
    explicit write is a counter RESET.
  - `unk_E2`, `unk_E5` similarly: their explicit writes are resets and their
    reads are elapsed-time tests, not value loads.
  - `unk_E4` -> **`var_stft_dwell_cnt`**, now understood: a free-running
    counter that `update_lambda_stft` clears whenever its entry conditions
    fail, and which must reach `0x40` before the controller is allowed to
    change closed-loop mode. A hold-off that stops the loop flipping state
    on a brief disturbance.

  **Lesson for future automated passes:** any "no writer found, therefore
  dead" conclusion about an address in `0x69-0x6A`, `0xAD-0xC5`, `0xC7-0xE7`
  or `0xE9-0xEE` must be checked against the `COUNTER_ARG` call sites first.
  This is the same class of mistake as the `tbs` error - a tool or
  assumption that cannot see one particular write mechanism.
- **update_ign_timing_blend (~E865-E9E9, partially traced this session)** - an ignition
  timing blend gated on `var_schedule_flag_41.3` via `tbs`+`beq` (runs on
  ticks where that bit tests clear; otherwise returns immediately). `tbs`
  is a destructive test-and-set (this session initially got that wrong,
  treating the gate as dead/always-true - see `var_schedule_flag_41`'s own
  ASM declaration comment for the correction), so this is a genuine
  self-re-arming one-shot gate: only the first call after each periodic
  unlock (iv6_4ms_process's 32ms sub-slot) runs the blend below. Two
  confirmed parts:
  1. **Init/reset path** (`var_flags_44.5` CLEAR - see the polarity
     correction below): seeds `var_unk_knock_12B`/
     `unk_12D`/`unk_12F` to `table_pim_unk_C154(dmatx_pim)/2` and zeroes
     `unk_129`/`unk_127`/sets `unk_AA=0xFF` - a first-run/reset baseline.
  2. **Normal path** (`var_flags_44.5` SET, every other tick): clamps
     `var_unk_knock_12B` between `unk_12F` and the PIM-table baseline
     (whichever's larger/smaller), using `var_diag_errors_5.0` purely as
     its own local "did the clamped value drop since last tick" flag - NOT
     knock-sensor-fault handling despite the flag's name (see
     `set_knock_sensor_err_flag`'s own header comment and the
     "fall-through code-reuse trick" architecture note - this was a
     genuine correction to what this entry said in an earlier session).
     That flag then selects `dmarx_ign_timing_fallback1/2` vs the primary
     `dmarx_ign_timing`/`dmarx_ign_timing_unk_166` for a
     multiply/table_ign_blend_weight blend feeding an `unk_129` accumulator, and later
     a `table_pair_interpolate` lookup (`unk_13F`/`unk_141`-selected)
     feeding `unk_127`, ending with a call to `decay_ign_ect_term`.
  **RESOLVED - the middle blend.** The ambiguity was the `mov`/`mult_rDrX`
  register flow, and it turns on the two `mov`s, both of which follow the
  src,dest order:
  - `mov d, x` -> **X = D**: the clamp excursion becomes the multiplier.
  - `mov x, d` -> **D = X**: takes `mult_rDrX`'s MSW output and DISCARDS its
    normal `D` result. That is a `>>16`, so with the preceding `mul a,#80h`
    the effective scale is `/512`, not the `/256` a casual read gives.

  Net: `blend term = (256 - timing) * |clamp excursion| / 512`, where the
  `neg a` inverts the timing byte (less timing -> more weight, forced
  non-zero when the timing byte is 0), and the timing source is
  `dmarx_ign_timing_unk_166` normally or `dmarx_ign_timing_fallback2` when
  the value was pulled DOWN by the clamp. A `cmp d,#0100h` deadband ignores
  excursions under 0x100 entirely.

  `unk_129` is a SIGNED accumulator with saturation at both rails: on
  overflow it loads `0x7FFF`, then if the sign flag is set the following
  `inc a`/`inc b` carries that to `0x8000` (-32768) - a neat way to saturate
  either direction using the flag that already carries the sign.

  **`var_unk_knock_12B`/`unk_12D`/`unk_12F` are a 3-stage delay line**, not
  three independent values: `12F <- 12D <- 12B <- new`, shifted once per
  qualifying tick at loc_E907. The init path seeds all three to the SAME
  PIM-table baseline - exactly how you initialise a delay line so it emits no
  false derivative on the first tick, which corroborates the reading. So like
  `calc_dmatx_pim`'s filter pair, this is fundamentally a rate-of-change
  structure.

  **RESOLVED: `sub_E832` is `decay_ign_ect_term`** - and tracing it exposed
  an inverted-polarity error in this entry and in the ASM comments it came
  from.

  The function itself is trivial: subtract 12 from `var_ign_ect_term` (was
  `unk_13D`), clamp at 0, and only while `var_flags_44.5` is SET.

  **The polarity correction.** Earlier comments - and points 1 and 2 above -
  said bit5 SET selects the init path and CLEAR the normal path. That is
  backwards. `tbbs` branches if SET, and three independent gates agree:
  - `tbbs bit5 -> loc_E890` takes the NORMAL path when set;
  - `tbbs bit5 -> loc_E7FD` SKIPS seeding `var_ign_ect_term` when set;
  - `decay_ign_ect_term` only decays when set.

  So **bit5 SET = normal running, CLEAR = init/seed.** Fixed in the ASM
  (four statements) and above. I had propagated the original error into
  `var_flags_44`'s own bit table as well, so that is corrected too.

  With the polarity right, the mechanism reads cleanly: `var_ign_ect_term`
  is seeded ONCE on the init pass from `table_ect_C185` (scaled through
  `scale_by_nv_trim_o2`, which folds in the O2-learned NV trim) plus `table_ect_C17C`,
  then bleeds down at 12 per tick while running, feeding an `add` into the
  timing sum. A warm-up ignition correction that fades out - the
  ignition-side counterpart of CPU2's enrichment decay.

  **Still not traced:** `table_ign_blend_weight`'s real-world meaning (the second lookup
  at loc_E92B feeding `unk_127`).

### `calc_transient_terms` (was `sub_E76D`) and its two maps

The last "units unknown" item, and the XDF only half-helps here - unlike the
PIM scaling, which it cracked outright. It *does* cover both maps, at 0xC00C
and 0xC0CD, exactly +6 from the ASM labels because the ASM label points at
the 6-byte map header and the XDF at the data. Both entries are titled
"Unknown", with identity scaling and blank axis labels - so the XDF's author
did not know their units either. What it does confirm is the x-axis (RPM) and
the dimensions, and those match the ASM headers exactly: 17x11 and 3x11.

Their structural role, though, is now solid. `calc_transient_terms` is
reached only from `calc_dmatx_pim`'s `var_flags_47.6` branch - i.e. only
during a large positive TPS delta - and runs two cascaded lookups:

- **`map_transient_mag`** (was `map_c006`): RPM x scaled-TPS-delta. Its
  result is returned in X and immediately multiplied by
  `var_pim_trim_scale`, so it lands in the pressure domain. It sets how FAR
  to correct. Values fall with RPM and rise with the other index, saturating
  near ~94 - consistent with "less correction needed as the manifold fills
  faster", though that reading is inference, not established units.
- **`map_transient_gain`** (was `map_c0c7`): RPM x (stage 1 / 8). Its result
  is left in `var_temp_7A`, which the caller hands to
  `signed_proportional_update` as the step size. It sets how FAST the
  estimator chases the transient.

So the pair is the **transient-response calibration for the whole
manifold-pressure estimator** - the maps to touch if tip-in response needs
changing. That is a genuinely useful answer for tuning even without absolute
units.

One caution recorded in the ASM: `var_temp_7A` is shared scratch
(`map_rD_rX_interpolate` uses it internally too), so it carries stage 2's
result only across the short window between the call and
`signed_proportional_update`.

### `loc_FC38` resolved: `validate_nv_trim_pim`

The other long-standing "not deep-dived" item on the CPU1 list, and it turns
out to be the missing half of a symmetric pair. `divide_d_by_x` calls the two
back to back, right after `calc_dmatx_pim`:

| Routine | Validates | Against |
|---|---|---|
| `validate_nv_trim_pim` (was `loc_FC38`) | `var_nv_trim_unk_98` (PIM/baro) | `nv_98_limits` = 0x64/0x37 |
| `validate_nv_trim_o2` (was `sub_D2C5`) | `var_nv_trim_unk_96` (O2-learned) | `nv_96_limits` = 0x80/0x00 |

Both wipe ALL of NV RAM through `clear_nv_ram` when their value fails its
bounds check. `byte_C3BD` renamed `nv_98_limits` to match - it is the same
limits pair used when *writing* that trim in `adc_handler_pim`, so it belongs
to the variable rather than to either call site.

`validate_nv_trim_pim` also does a second job its twin does not: it derives
`var_pim_trim_scale = ((trim or 0x50) / 2) / 0x61`, saturated - the factor
that puts `calc_dmatx_pim`'s throttle-derived pressure estimate into real PIM
units. **Correction:** `var_pim_trim_scale`'s declaration credited that
computation to `adc_handler_pim`. It is `validate_nv_trim_pim` that computes
it; the routine merely *sits inside* `adc_handler_pim`'s address range, which
is why IDA labels its internal branches `adc_handler_pim+NN`. It is not
reached by fall-through - that handler jumps over it to `loc_FC57` - and its
only entry is the `jsr` from `divide_d_by_x`.

  **Related sweep done at the same time:** CLAUDE.md flagged that the shared
  math library's `mov s, x` comments carried the reversed-notation error and
  had never been swept. Ten of them said "SP = X"; `mov s, x` is `X = S`, and
  the very next instruction (`div d, x + 00h`, `ld a, x + 01h` ...) uses X as
  a stack frame pointer, which settles it. All ten fixed, plus two explicit
  reversals (`mov d, x` commented "D = X", `mov b, a` commented "B = A").
  Remaining `mov` comments in that library are descriptive ("Y = result"
  meaning what Y holds) rather than wrong, so they were left alone.

### CPU2 (D151803-9661)

All functions are now named (verified: every jsr/bsr call target,
indirect/computed call target, and interrupt vector table entry resolves
to a meaningful name - see "loc_ sweep" above). What's left is prose
documentation and a few specific loose ends, not "find the function" work.

**Not yet started:**
- **RESOLVED: serial_debug_check's framing.** The open question asked how a
  2-byte index accumulates across 4ms ticks. It does not - that premise was
  wrong. The index is assembled entirely inside one call, and it is **nine
  bits, not sixteen**:

      ld b, SIDR_SODR   B = received byte              -> index bits 7..0
      ld a, SSD         A = SSD bit 0, the 9th/parity
      and a, #01h           bit of the frame           -> index bit 8

  `D` is never loaded directly - it simply *is* A:B, built by those two
  loads, and `cmp d, #001Fh` tests the assembled value. `shl d` doubles it
  to a byte address, giving a reach of 0x000-0x3FE, which covers all of RAM
  (0x40-0x300). **That is why there is no bounds check: the encoding cannot
  address outside RAM.**

  Sequence, with the ECU as bus master: send 0xDA (the read-16 command),
  spin up to 14 times on SSD.7 (receive ready), drop the frame if SSD.6 is
  set, else assemble the index, look the word up and send it back MSB then
  LSB. SSD.6 reads as a receive-error flag given SSD.7 is "ready" and SSD.0
  is the 9th data bit - but that is inference from position and use, not
  something the ROM states.

  The 0xDA byte matches the gateway firmware's protocol notes in CLAUDE.md
  (0xDA read-16, 0xDB/0xDC write-16, 0xDD/0xDE write-8) and its "Denso is
  master, the SAMC21 answers" description. Only read-16 is implemented in
  this routine; the write commands are not handled here.

  Two curiosities recorded inline so nobody "fixes" them: `div d, #00h` is a
  deliberate divide-by-zero used as an inter-byte delay, and the `.db 41h`
  after the rom_version load is the opcode trick that swallows the following
  `shl d`, because rom_version is already a byte address and must not be
  doubled.

- **RESOLVED: `var_asr0n_shadow_126`'s bits 6/7** - and the answer was a
  hardware fact no amount of ROM reading would have produced. From Jon:
  **ASR0 read and write are different functions at the same address.**
  Writing ASR0 configures the DMA engine; reading it returns the latched
  timer value captured on an I/O transition. The "falling edge counter"
  name in the technical reference describes only the READ side.

  That explains the whole variable. Because a read does not return what was
  written, ASR0N cannot be read-modify-written - so the code keeps a
  software shadow of what it wrote, which is precisely what
  `var_asr0n_shadow_126` is. Every site has the identical shape:

      ld a, var_asr0n_shadow_126     recover what we last wrote
      and/or a, #mask                change one bit
      st a, var_asr0n_shadow_126     keep the shadow current
      st a, ASR0N                    push it to the register

  So bits 6/7 are **DMA control bits**, not counter bits. The standing note
  that they "don't fit ASR0N's documented falling-edge-counter role" was
  comparing them against the wrong side of the register.

  Two independent corroborations from the ROM itself: the shadow pattern
  appears at *every* site without exception, and `loc_D516` performs a
  deliberate clear-then-set pulse on bit 7 - writing 0 then 1 to the
  register while the shadow records only the final value. That is a command
  to a DMA engine; it is meaningless as a counter write.

  `docs/toshiba-8x-technical-reference.md`'s ASR0 register table now carries
  this asymmetry as a note, since it applies to any D8X disassembly, not
  just this ROM.

  **Lesson worth keeping:** this sat open for several sessions labelled
  "needs hardware probing", and it did - but the probe needed was one fact
  about the silicon, not a scope trace. Where a register's behaviour refuses
  to fit its documented role, the documentation describing only one access
  direction is now a hypothesis worth testing early.

**Resolved - broad prose-documentation gap:** every real (non-math-library)
function in CPU2 now has gold-standard header prose, confirmed by a
systematic pass this session (every call-target label checked for a header
either preceding or immediately following its label - a few earlier
"missing" hits from a divider-only heuristic turned out to already be
documented with the header written right after the label instead of
before it, e.g. `generate_vf_PORTA_4`, `calc_rpm`, `drive_DOUT2_tvsv`,
`check_io_inputs`). The generic math/interpolation library
(`divide_rD_*`/`mult_*`/`table_*`/`map_*`/`clamp_rD`) remains intentionally
undocumented beyond its existing per-line comments and self-explanatory
names, per this project's established convention (see "Previously
completed" above). Zero `sub_`-prefixed labels remain in the file. What's
left for CPU2 is only the two items below - no more "sweep the whole file
for gaps" work needed.

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
- **DMA buffer offsets - two separate formulas, one per direction:**
  CPU2-tx/CPU1-rx: `CPU1_addr = CPU2_addr + 0xDA` (word-sized variables
  have a 1-byte padding discrepancy in one region - see "CPU1<->CPU2 DMA
  cross-reference" below). CPU1-tx/CPU2-rx (the opposite direction):
  `CPU1_addr = CPU2_addr + 0x13B`, confirmed via four independent pairs
  (tps/ect/pim/battery) with no padding discrepancy found so far - see
  "dmarx_unk_D6 resolved" above.
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
- **Fall-through code-reuse trick (a second ROM-space-saving idiom, distinct
  from variable-aliasing above):** `set_knock_sensor_err_flag`,
  `check_knock_sensor_err_flag`, and `negate_rD` are three separate,
  separately-named functions with no `ret` between them - each one's body
  falls straight into the next's. `set_knock_sensor_err_flag` (`setb bit0,
  var_diag_errors_5`) falls into `check_knock_sensor_err_flag`'s test of
  that same bit (now guaranteed set) which falls into `negate_rD` - so
  calling "set the knock error flag" also unconditionally negates D.
  `check_knock_sensor_err_flag` alone conditionally negates D based on
  whichever call set (or didn't set) the flag earlier in the same
  computation. Despite the name, `var_diag_errors_5.0` is reused repo-wide
  as a generic "did we take the absolute value of D" remember-bit, not
  something knock-sensor-specific outside the actual knock subsystem -
  confirmed at three unrelated sites (`ramp_limit_inj_pw`'s loc_DBB5,
  `calc_iscv`'s threshold check per idle_control_system.md, and CPU1's
  `update_ign_timing_blend`). Watch for this shape - `jsr set_knock_sensor_err_flag`/
  `jsr check_knock_sensor_err_flag` immediately before or after a
  subtract/compare on D - anywhere a stray-looking "knock" flag call
  doesn't fit the surrounding computation's subject matter.
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
