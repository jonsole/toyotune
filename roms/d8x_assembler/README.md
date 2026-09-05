# D8X Assembler (`asm_d8x.py`)

A Python reimplementation of the D8X cross-assembler, replacing `Tasm32.exe`
(`roms/bin/Tasm32.exe`) as the tool the Makefiles actually invoke to turn a
`.ASM`/`.asm` source file into a ROM image. `Tasm32.exe` is kept only as a
reference/cross-check binary; nothing in the build path calls it any more.

This assembler has been confirmed to produce byte-identical output to real
`Tasm32.exe` across every buildable ECU source in the repo, including
sparse-layout personal-tune sources like `Jon_ST205_ECU`'s (see
`roms/verify_assembly_match.py`, the tool used to confirm that).

## Files

| File | Responsibility |
|---|---|
| `asm_d8x.py` | Orchestration: line parsing, the two-pass label-resolution loop, output emission, CLI entry point |
| `lexer.py` | `MathEvaluator` — tokenizes and evaluates operand/argument expressions (`+ - * / << >> & \|`, parens, hex/binary literals, label lookups) |
| `directive.py` | `.org`, `.block`, `.db`, `.dw`, `.end`, `.equ`, `.locallabelchar` |
| `instruction.py` | The full D8X opcode table and operand-to-encoding matching |
| `macro.py` | `.macro`/`.endm`/`.local` — whole-line macro expansion |
| `define.py` | `.define` — C-preprocessor-style text macros |
| `tests/` | `unittest` suite for all of the above; run with `python -m unittest discover -s tests -t .` from this directory |

## Command line

```
python asm_d8x.py [-f {bin,obj}] [-p XX] input output [listing]
```

- `input` — source `.ASM`/`.asm` file
- `output` — output binary path
- `listing` (optional) — `.lst` listing path (address/opcode-bytes/source, one line per source line — see "Listings" below)
- `-f`/`--format` — only `bin` is implemented; `obj` is accepted by the parser but not implemented and will raise
- `-p`/`--fill` — hex fill byte (e.g. `5F`) written into the *gaps between* `.org` blocks in the output, matching `Tasm32.exe`'s `-f<xx>` flag. Default `00`. Nothing is emitted before the first block's own address — only gaps *between* blocks are filled.

Exit code is `0` on success, `1` if any diagnostic was reported.

This is what `roms/<family>/makefile.lib`'s `ASM`/`ASMFLAGS` invoke (with
`-p 5F`, matching `Tasm32.exe`'s historical `-f5F` behavior); see the
top-level `README`/`CLAUDE.md` "Building a ROM" section for the `make.exe`
targets that wrap it.

## Pipeline

Each call to `Assembler.Assemble()` runs the source through four stages, in
this order:

1. **`.define` expansion** (`_ExpandDefines`) — pure text substitution, C-preprocessor style. Runs first so a `.define` can be used inside a macro body or as a macro argument.
2. **`.macro` expansion** (`_ExpandMacros`) — whole-line/whole-block expansion of `.macro`/`.endm` invocations.
3. **Two-pass label resolution** (`_RunPass`, called from `_Assemble`'s pass loop) — see below.
4. **Output emission** — the stable pass's byte blocks, in address order, written to `output` with `-p`'s fill byte in the gaps between blocks.

Stages 1 and 2 are each a one-time source-to-source transform (they don't
depend on any label's resolved address), so they run once, ahead of the
address-resolution passes, rather than being repeated on every pass.

### Two-pass (up to `MAX_PASSES = 10`) label resolution

Pass 1 walks the source once, assigning every label the PC it's defined at.
A forward reference to a label not yet seen falls back to a placeholder (the
current PC) rather than erroring — see `AssemblerPass.ParseExpression`.

Because some instruction encodings are chosen by the *size* of an operand
value (e.g. `ld a,nn` 8-bit absolute vs. `ld a,nnnn` 16-bit absolute — see
`instruction.py`'s `opcodes['ld']['a']` and how `Assemble()` tries `nn`
before `nnnn`), a label resolved from a pass-1 placeholder can pick the
wrong-sized encoding, which shifts every later label's address. Each
subsequent pass re-resolves against the previous pass's label table;
`AssemblerPass.SetLabel` tracks whether any label's value actually changed
(`LabelsChanged`), and the loop in `_Assemble` keeps re-running fresh passes
until a pass changes nothing, or `MAX_PASSES` is reached (reported as a
normal diagnostic, not a crash). In practice every ROM assembled so far
converges in 2 passes; a 3rd is only needed when a forward-referenced
operand's encoding size was guessed wrong on pass 1 (see
`tests/test_assembler.py`'s `TestLabelConvergenceAcrossPasses` for a worked
example).

The listing (if requested) is only ever generated from the pass that turns
out to be the stable one — earlier passes' listing output is discarded
(`listing.seek(0); listing.truncate()` before each retry).

### Error handling

A per-line problem (bad operand, unknown opcode, out-of-range value, ...)
raises `AssemblyError`/a subsystem-specific error (`instruction.InstructionError`,
`directive.DirectiveError`, `macro.MacroError`, `define.DefineError`), which
`_RunPass`/`_ExpandMacros`/`_ExpandDefines` catch and record as a
`Diagnostic` — assembly *keeps going* past a bad line so later, independent
errors are still reported in one run, rather than the user fixing errors
one at a time. Only an unexpected (non-`AssemblyError`) exception aborts a
pass early, and is recorded as an `'internal'`-level diagnostic rather than
silently propagating. `Assembler.diagnostics` (and `.errors`, its count) is
authoritative: zero means assembly succeeded and `output` was written; any
diagnostic means it wasn't, and `output` was closed but not populated with
anything you should trust.

## Directive reference

All directives are dot-prefixed and case-insensitive.

| Directive | Effect |
|---|---|
| `.org nnnn` | Set the program counter. Starts a new output block. |
| `.block n` | Advance the PC by `n` bytes without emitting any — ends the current output block and starts a new one `n` bytes later, so the reserved space becomes a gap filled with `-p`'s fill byte in the output, the same as an `.org` gap |
| `.db a,b,c,...` | Emit each expression as a byte (range-checked -128..255) |
| `.dw a,b,c,...` | Emit each expression as a big-endian word (range-checked -32768..65535) |
| `.end` | Stop assembling the rest of the file (an explicit end-of-source marker) |
| `name .equ expr` | Bind `name` to the *value* of `expr`, evaluated once — not to the current PC. Requires a label; requires exactly one argument. |
| `.locallabelchar c` | Parsed but a no-op (`HandleLocalLabelChar`) — accepted for source compatibility, has no effect |

Expressions (used by `.org`/`.block`/`.db`/`.dw`/`.equ` arguments and every
instruction operand) support `+ - * / << >> & \|`, parentheses, unary
`+`/`-`, decimal (`123`), hex (`$1F` or `1Fh`), binary (`101b`), and label
identifiers — see `lexer.py`'s `MathEvaluator`. `/` is true (float) division,
matching the historical `eval()`-based implementation this replaced.

## `.macro` / `.endm` / `.local`

Whole-line macros, expanded before any address resolution happens:

```
clear_a .macro
        clr     a
        .endm

        .org    0
        clear_a         ; expands to: clr a
```

- The macro's **name is the label on the `.macro` line** (`clear_a` above), matching how `.equ` also uses the label field for the thing being defined.
- **Positional parameters are `]1`..`]9`, not `#1`..`#9`.** This is a deliberate deviation from classic TASM's usual convention: this ISA's immediate-addressing operands are themselves written `#nn` (e.g. `ld a,#5`), so a `#`-based placeholder would be ambiguous with a literal immediate value written inside a macro body.
- **`.local sym1, sym2, ...`** declares labels that get a unique suffix generated per expansion (`sym__macroname_N`), so the same macro can be invoked more than once without its internal labels colliding:

```
countdown .macro
        .local  loop
        ld      a,#]1
loop:   dec     a
        bne     loop
        .endm
```

- A label on the invocation line becomes the label of the macro's first expanded line (`entry: clear_a` labels the `clr a` it expands to).
- Macros may invoke other macros (expansion is iterative, up to `macro.MAX_EXPANSION_DEPTH = 10` levels, guarding against a macro that invokes itself).
- Diagnostics/listings for an expanded line report the *original invoking* source line number, not a synthetic one — `_ExpandMacros` returns a parallel `line_nums` array alongside the expanded line list for exactly this reason.
- A source line untouched by macro expansion is passed through as its *exact original text* (not reformatted/reconstructed) — only genuinely macro-generated lines go through `_ReconstructLine`. This means a macro-free file's listing output is byte-identical to what it would have been without any of this machinery.

## `.define`

C-preprocessor-style text macros — substituted as raw text *before*
expression evaluation, which is what lets a `.define` stand in for part of
an expression rather than only a whole value (the thing `.equ` can't do):

```
        .define BIT0 1
        .define BIT1 2
        .define MASK (BIT0|BIT1)

        ld      a,#MASK         ; expands to: ld a,#(1|2)
```

Function-like defines take real per-call arguments (nested calls as
arguments work too, e.g. `ADD(DOUBLE(2),1)`):

```
        .define DOUBLE(x) (x*2)

        ld      a,#DOUBLE(3)    ; expands to: ld a,#(3*2)
```

- **No space between name and `(`** in the *declaration* is what makes it function-like (`.define MASK (BIT0|BIT1)` — space present — is object-like whose replacement text happens to start with a paren; `.define DOUBLE(x) ...` — no space — is function-like), matching C's own convention. At a *use* site, whitespace before `(` is fine either way.
- A function-like name used without a following `(...)` is left as a literal, unexpanded identifier — matching C, this usually then fails downstream as an unknown identifier, which is the expected outcome (same as it would be in C without some other definition of the bare name).
- Recursive/self-referential expansion is guarded the way C guards it: a define's own name reappearing inside its own (possibly nested) expansion is left unexpanded rather than re-substituted, so a circular definition can't loop forever — it just produces a literal name that then fails as an unknown identifier when evaluated.
- `.define` runs *before* `.macro` expansion, so a `.define` can be used both inside a macro body and as a macro invocation argument.
- Each `.define` declaration line is blanked out in place (not removed), so — unlike macro expansion — line numbers for every other line in the file are completely unaffected; no line-number remapping is needed for this stage.

**Caveat:** the `]1..]9` macro-parameter syntax and the exact `.define`
token-substitution semantics above are this project's own design, informed
by (but not verified against) real TASM's actual macro dialect — there is
no TASM macro/preprocessor manual in this repo to confirm against, and
nothing in the repo used macros before this assembler existed. If you ever
port a macro from another TASM-built project, expect to translate its
syntax rather than copy-paste it.

## Listings

With a `listing` argument, each source line produces one (or more, if its
emitted bytes exceed 4) line of the form:

```
LLLLL  AAAA  XX XX XX XX   source text
```

`LLLLL` is the original source line number (see the macro-expansion note
above re: how this stays meaningful even for macro-generated lines), `AAAA`
is the PC that line started at, and the hex bytes are its emitted bytes (up
to 4 per listing line; a longer instruction/`.db` continues on subsequent
listing lines with a blank source-text column). Compare two listings with
`roms/verify_assembly_match.py`, which re-anchors the comparison on the
address column rather than diffing raw text — see the top-level `CLAUDE.md`
"Working with the disassembly" section for why a naive diff is the wrong
tool here (one real edit shifts every downstream address reference by the
same amount).

## Known limitations

- Only `-f bin` output is implemented; `-f obj` is accepted by the CLI but raises `AssemblyError("Unsupported output format 'obj'")`.
- `.block n` emits *no bytes at all* — it just advances the PC, which (via `AssemblerPass.SetPc`) ends the current output block and starts a new one `n` bytes later. So a `.block`'s reserved space isn't zero-filled in the output; it's a real gap between two blocks and gets `-p`'s fill byte at output time, exactly like an `.org` gap.
- Naming is inconsistent about case sensitivity: opcodes, directive keywords (`.org`, `.macro`, `.endm`, `.local`, `.define`, ...) and **macro names** are all matched case-insensitively (lower-cased for comparison — `macro.MacroProcessor` stores and looks up by `name.lower()`), but **labels and `.define` names are case-sensitive** (`lexer.py`'s `Identifier` lookup and `define.DefineProcessor` both match the raw name as written). `Countdown` and `countdown` invoke the same macro; `.define FOO 1` and a later use of `foo` are two different, unrelated identifiers.
- `.define`'s substitution is a simplified model of C's preprocessor: a definition's body is expanded recursively as it's spliced in, but the *result* is not re-scanned as one combined string afterward — two macro invocations that would only form a valid name once concatenated across a substitution boundary won't be recognized. This doesn't arise in normal use.
