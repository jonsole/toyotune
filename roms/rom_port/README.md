# rom_port — carrying symbol names between sibling ECUs

Toyota shipped the same firmware across several cars with small differences.
`D151803-9651` (SW20 MR2 CPU1) and `D151804-0461` (ST205 Celica CPU1) share
most of their code; so do their CPU2 partners `-9661` and `-0471`. Once one
ROM has been reverse engineered, most of that work is transferable — this
tool transfers it.

It only ever renames. It never changes an instruction, and the output must
always assemble to a byte-identical binary. Verify that every time; see
"Verifying" below.

## Why not just match addresses

Because the addresses do not line up, and the RAM map does not either. Between
9651 and 0461 it shifts piecewise — 0 below ~0x0B3, −4 across ~0x0B4–0x0CC,
−6 across ~0x0CE–0x14C, −8 across ~0x14F–0x194, back to −6 by 0x1DE. Every
match here is made on what the code *does*.

## Usage

```
python port_rom.py scope   SOURCE.asm TARGET.asm          # how well do they match?
python port_rom.py plan    SOURCE.asm TARGET.asm -o p.json
python port_rom.py apply   TARGET.asm p.json
python port_rom.py offsets CPU1.asm CPU2.asm              # this pair's DMA offsets
```

`plan` writes nothing. `apply` is the only command that modifies a file, and
by default it applies only `NEW` renames — symbols the target has not named
at all. Renames that would *overwrite* an existing name are reported and
skipped unless you pass `--include-updates`, because those are a semantic
claim and need a human (see "Updates are not free" below).

Add `--windows` for the second matcher, which reaches further but on weaker
evidence; pair it with `--min-votes 8`.

Work on the `Claude/` copy of a ROM, not the parent `.ASM` — see CLAUDE.md.

## The two matchers

**Exact function signatures** (default). Normalise every function to its
sequence of `(mnemonic, operand)` pairs with symbol names replaced by a
placeholder and immediates kept. Two functions whose sequences are identical,
and which are each unique within their own ROM, are the same function. Walk
the pair instruction by instruction: symbols at the same position are the same
symbol. 9651→0461 matches 83 functions this way.

**Instruction windows** (`--windows`). Index every run of 10, 8 and 6
instructions. Where a run is unique in both ROMs and identical between them,
the symbols line up. This needs no function boundaries, which matters because
IDA folds a lot of code into giant chunked routines where the function matcher
is blind. `unk_23C` in 0461 was only identified as `dmarx_iscv_duty` this way —
and that identification is what led to the chargecooler pump.

Fuzzy function matching exists in `matchers.py` but feeds `scope` only, never
renaming. A 0.93 fuzzy match once paired 9651's `adc_handler_pim` with a 0461
function that was not it, while 0461's real `adc_handler_pim` sat elsewhere.

## Cross-checking a port

Reference counts. For each rename, count operand references to the old name in
the source ROM and to the new name in the target. They should agree if the
mapping is right. On the 228-rename windowed batch, 221 matched exactly and
all 228 were within 2 — the small differences being real divergence between
the ROMs.

## Names that embed an address

Roughly a third of the hand-names in these ROMs carry a hex address —
`inc_cnt_187`, `table_ect_C185`, `var_ect_unk_194`. Ported verbatim they
embed the *source* ROM's addresses in the *target*, which is worse than
leaving the symbol unnamed. `adapt_name()` rewrites them, but only where it
can justify the rewrite:

- **the symbol's own address** — always, and this takes precedence over every
  other rule.
- **an address the matched source function demonstrably references** — this is
  what makes `inc_cnt_187` (which names the counter it increments, not itself)
  portable. Only available with exact-function evidence.
- **an address that maps to itself** — a no-op.

Everything else is refused and reported. That is deliberate: a refused name
costs a manual decision, a wrongly rewritten one is a lie that reads as a
fact.

## Traps this encodes

Each of these reached a real source file before being caught.

**An operand can be an expression.** IDA writes some as
`#(table_knock_retard_step-1)`. Treating the parenthesised expression as one
symbol produced the label definition `(table_knock_retard_step-1):` and an
assembly that would not converge. Operands are tokenised on brackets and
arithmetic, and every rename target is validated as an identifier.

**Own-address rewriting must beat the identity map.** 9651's `var_cnt_CE`
belongs at `var_cnt_C8` in 0461. An unrelated identity entry for 0xCE left the
name untouched, and only the collision with the real `var_cnt_CE` revealed it.

**Not every uppercase hex run is an address.** `igf_count_rpm_lt_3000` names an
RPM threshold and was briefly rewritten to `igf_count_rpm_lt_F6B3`. Fragments
must fall in RAM (≤0x3FF) or ROM (0xC000–0xFFFF); two-digit runs that are not
an address anywhere in the pair, like the `32` in `divide_rD_32`, are left
alone.

**`adc` is three valid hex digits.** Fragments are matched uppercase-only.

**A name can carry a CPU2 address.** `dmarx_max_retard_23B_161` holds a CPU1
address *and* a CPU2 one. Rewriting the CPU2 half with a CPU1 map silently
lands on the wrong variable — see the next section. Any 3–4 digit fragment
that is not the symbol's own address is refused.

**A target name may already exist.** A rename to `adc_handler_pim`, a name
0461 already used, produced a duplicate label. Checked against every label and
data symbol in the target.

**Renames must be simultaneous.** A batch can legitimately contain A→B while
B→C; applying those in sequence merges two symbols. One regex pass.

**Two targets claiming one source, or two sources resolving to one name**, are
both contradictions and both rejected.

## Updates are not free

A rename that overwrites an existing name is *not* automatically an
improvement. Two very different things look identical to the matcher:

- the source ROM genuinely learned a better name — `var_unk_knk_12D` →
  `var_pim_est_fast`, worth taking.
- the two ROMs actually differ there, and the target's name is the correct
  one. 9651's ADC slot 8 is `adc_handler_trac_tps`, which range-checks the
  secondary throttle. 0461's slot 8 is a two-instruction stub that stores the
  reading and discards it, correctly named `adc_handler_unk_fd7f`. The
  matcher pairs them positionally and proposes the rename; taking it would
  assert that 0461 handles traction control, which it does not.

Hence `--include-updates`. Read them individually.

A related failure with no automated defence: a ported name freezes a snapshot
of the source ROM's understanding. `knock_unk_E712` and
`some_knock_averaging_calc` were ported correctly, then 9651 learned they were
`update_pim_est_fast`/`update_pim_est_slow` — manifold pressure, not knock —
and the copies in 0461 were quietly wrong until the next port. Re-run the port
after significant work on the source ROM.

## Inter-CPU DMA offsets

`offsets` derives them from the pair's own `dmatx_X`/`dmarx_X` name pairs.
**They are a property of the ECU pair, not of the family** — CLAUDE.md used to
give the MR2 numbers as though they were general:

| pair | CPU2 → CPU1 | CPU1 → CPU2 |
|------|-------------|-------------|
| `D151803-9651`/`-9661` | `+0xDA` | `+0x13B` |
| `D151804-0461`/`-0471` | `+0xD1` | `+0x133` |

Nine and eight bytes apart — enough to land inside a neighbouring variable and
return something plausible. Take the modal offset: a 16-bit variable whose name
sits on the other byte of the pair will disagree by one.

## Verifying

Renames are inert by construction, but that is a claim to check:

```
python ../d8x_assembler/asm_d8x.py -p 5F TARGET.asm out.bin out.lst
cmp out.bin <the shipped .BIN>
python ../verify_assembly_match.py before.lst out.lst      # "Total real edit regions: 0"
```

Use `verify_assembly_match.py`, not a raw diff — one real change shifts every
downstream address and a naive diff reports hundreds of phantom differences.

## Tests

```
cd roms/rom_port && python -m unittest discover -s tests -t .
```

Every test corresponds to one of the traps above.
