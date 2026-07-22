# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A reverse-engineering and ROM-tuning workspace for Toyota ECUs built on the **Toshiba/Denso 8X (D8X)** microcontroller — a proprietary MCU with an instruction set derived from the Motorola M68HC11. The repo covers disassembly, annotation, reassembly, and tuned-ROM production for multiple Toyota engine families.

This is a git repo (`origin` = `github.com/jonsole/toyotune.git`, also serving a GitHub Pages site off `master` — see `_config.yml`/`README.md`). There is no build system in the CI sense; "building" means reassembling one ECU's `.ASM` source back into a flashable binary via `make`.

## Repository layout

The repo has three top-level areas: **`roms/`** (disassembly/ROM-tuning workspace — the vast majority of the content and of this guide), **`hw/`** (Toyotune interface hardware), and **`sw/`** (IDA Pro installs).

### `roms/`

- **Engine family directories** (`1G-GTE`, `1G-GZE`, `1JZ-GTE`, `1UZ-FE`, `3S-GE`, `3S-GTE`, `3VZ-FE`, `4A-GE`) — each contains one subdirectory per ECU part number (e.g. `D151803-9651`), plus person-named subdirectories (`Jon_ST205_ECU`, etc.) for individual custom-tune branches derived from a base ROM.
  - Within an ECU directory: `.ASM` (IDA-exported/hand-annotated disassembly), `.bin`/`.BIN` (ROM images), `.idb`/`.id0`/`.id1`/`.nam`/`.til` (IDA Pro database files — open the `.idb` in IDA to browse/edit the annotated disassembly), `.XDF` (TunerPro tune-definition files for map editing), `.xlsx` (extracted/diffed calibration maps), `Makefile`.
  - `output/` — build artifacts: `build.txt` (assembler log), plain `.bin`/`.lst`, plus `output/toyotune/` (32K-padded image for the Toyotune flasher) and `output/techtom/` (scrambled/XOR-encoded image for the Techtom tool).
- **`- Copy` files** (e.g. `D151803-9651 - Copy.ASM`) are older, superseded snapshots (dated ~2018) kept alongside the live working file (dated 2020+). **Never treat a `- Copy` file as the one to edit** — always confirm which is current by timestamp/content before touching either.
- **`roms/docs/`** — reverse-engineering notes on the D8X MCU and on specific ECU subsystems (currently written against CPU1 of `D151803-9651`, 3S-GTE):
  - **`session_journal.md`** — the living progress log for the `D151803-9651` reverse-engineering effort. **Read this first when resuming work on that ROM.** It tracks completed subsystems and their key renames (`sub_XXXX` → meaningful name), a pending-work list of untouched chunks/functions, a key-variable reference table, and architecture notes (CPU1/CPU2 split, inter-CPU DMA, timer resolution, NE pulse geometry). Update it as new subsystems/functions are understood — append to "Completed subsystems", move items out of "Pending work", and add new renames to the variable reference table, rather than letting this knowledge live only in `.ASM` comments or chat history.
  - `knock_mcu_update.ASM` — a fully-annotated extracted routine (from `D151803-9651`) kept as a reference example of the target comment/annotation style: purpose header, per-branch behavior explained, flag semantics spelled out, cross-refs preserved. Use this as the bar for quality when annotating other functions.
  - `toshiba-8x-technical-reference.md` — full instruction set, opcode matrix, registers, addressing modes for the D8X.
  - `toshiba-8x-reference-part1.md`, `-part2-appendix.md` — extended/appendix material for the above.
  - `adc_system.md`, `ignition_system.md`, `knock_sensor_system.md`, `idle_control_system.md`, `fuel_calculation_system.md` — polished, per-subsystem write-ups (ADC channel scanning, ignition timing/dwell scheduling, per-cylinder knock retard learning, idle speed control, base injector pulse-width calculation) distilled from the session journal once a subsystem is fully understood, including key symbol names, timing units, and hardware pin mappings.
- **`roms/bin/`** — compiled toolchain binaries used by the Makefiles: `Tasm32.exe`/`tasm8x.bat` (Table-driven Assembler for the D8X, the actual `ASM` invoked by `make`), `TASM8x.TAB` (opcode table), `checksum.exe`, `scramble.exe`, `descramble_brute.exe`, `descramble_brute2.exe`, `make.exe`.
- **`roms/checksum/`** — Visual Studio C++ source project (`.sln`/`.vcxproj`) for `checksum.exe`. Rebuild in Visual Studio if the tool needs changing; the checked-in `.exe` in `roms/bin/` is what Makefiles actually call. (Source projects for `scramble.exe`/`descramble_brute*.exe` were not carried over in this workspace transfer — only their compiled `bin/` binaries are present.)
- **`roms/d8x_assembler/`** — a tested **Python reimplementation** of the D8X assembler (`asm_d8x.py`, `instruction.py`, `directive.py`, `lexer.py`; test suite under `roms/d8x_assembler/tests/`, run via `python -m unittest discover -s tests -t .` from `roms/d8x_assembler/`). **Prefer `asm_d8x.py` over `roms\bin\Tasm32.exe` for reassembling/verifying `.ASM` edits** during a reverse-engineering or tuning session — it's faster, gives clearer per-line error messages, and exits non-zero on failure. It is independent of the Makefile-driven `tasm32` toolchain and not wired into any Makefile: it only produces a plain `.bin` (no 32K Toyotune padding, no Techtom scramble/XOR, no checksum patching), so **producing an actual flashable image still requires `make.exe rom_toyotune`/`rom_techtom`** (tasm32-based, see "Building a ROM" below).
- **`roms/verify_assembly_match.py`** — the canonical copy of the `.lst` comparison script (see "After editing" below); an identical duplicate also lives at `roms/3S-GTE/D151803-9651/Claude/verify_assembly_match.py`.

### `hw/`

- **`hw/toyotune_lv_2p0/`** — hardware design for the Toyotune interface board, v2.0: `pcb/` (Eagle `.sch`/`.brd`, OSH Park fab files, BOM) and `cpld/` (`toyotune_3s_lvc_dil28`, `toyotune_3s_lvc_plcc32` VHDL projects for the two package variants).

### `sw/`

- **`sw/IDA/`** — IDA Pro installations (`ida490free`, `ida500free`) used to open/edit the `.idb` files.

## Building a ROM

Each ECU directory's `Makefile` includes the shared `<family>/makefile.lib`, which defines the actual rules. From an ECU directory (e.g. `roms/3S-GTE/D151803-9651`):

```
make.exe rom            # assemble + checksum -> output/<name>.bin
make.exe rom_toyotune    # + pad to 32K for Toyotune flasher -> output/toyotune/<name>.bin
make.exe rom_techtom     # + scramble/XOR-encode for Techtom -> output/techtom/<name>.bin
make.exe clean           # remove output/
```

Use the `roms/bin/make.exe` on PATH (or invoke it explicitly), not a generic `make`, since these Makefiles use Windows-specific `mkdir`/`rmdir` logic (`win_path`, `make_dir`, `rm_dir` in `makefile.lib`).

From a family root (e.g. `roms/3S-GTE/Makefile`), the top-level `ROMS` list fans out the same targets across every ECU in that family: `make.exe rom`, `make.exe rom_toyotune`, `make.exe rom_techtom`, `make.exe clean`.

Build errors and assembler pass statistics land in `output/build.txt` (appended, not overwritten — check timestamps/tail when diagnosing a failed build). A `.lst` listing file is produced alongside the `.bin` showing address/opcode-byte/source correlation.

Toolchain requires `TASMTABS` to point at the directory containing `TASM8x.TAB` — this is exported from `makefile.lib` as an absolute path and will need updating if the repo is relocated.

## Working with the disassembly

- Symbol names are being progressively reverse-engineered from IDA's auto-generated defaults (`sub_C59B`, `unk_1C`, `loc_E16D`) into meaningful names (`iv6_ne_process`, `var_ign_advance_raw`, `CPR0`) as their purpose is understood. When editing `.ASM` files, match this convention: `sub_`/`loc_`/`unk_` prefixes mean "not yet understood," and renaming one should reflect a confirmed understanding of hardware behavior, not a guess.
- Cross-reference comments (`; DATA XREF: ...w`/`...r`) are IDA-exported and describe where a symbol is written/read from elsewhere in the same ROM — keep them accurate when moving/renaming code, since they're a primary navigation aid across a ~14k-line disassembly.
- The `roms/docs/*.md` files are the authoritative write-ups of subsystem behavior already reverse-engineered; consult them before re-deriving how ADC scanning, ignition scheduling, or knock retard already work, and extend them (rather than duplicating in comments) when new subsystems are understood.
- `D151803-9651` (3S-GTE, CPU1) is the most heavily annotated/documented ECU so far — treat it as the reference implementation when a similar routine appears in another engine family's disassembly.
- Before starting new reverse-engineering work on `D151803-9651`, read `roms/docs/session_journal.md` for current status (what's done, what's pending, established variable names) so renames stay consistent with prior sessions and don't duplicate completed work.
- **Editing `.ASM`/`.asm` files with exact-string tools:** these files (Latin-1/CP1252 encoded) contain stray single control bytes (e.g. `0x18`) immediately before the trailing type letter in `; CODE XREF: ...+Nj/p/r/w/o` comments — invisible in normal rendering but present in the raw bytes, so an old_string reconstructed from what you see may fail to match. If an edit on a line ending in one of these xref comments doesn't match, either match only up to just before the final offset+letter, or target a line without a trailing xref comment instead.
- `D151803-9651` (CPU1) and `D151803-9661` (CPU2) each have a `Claude/` subfolder containing a working copy of the `.ASM` (`Claude/D151803-9651.asm`, `Claude/D151803-9661.asm`) that is *ahead* of the parent directory's `.ASM` (the one the Makefile actually builds) in renames/comments from ongoing reverse-engineering sessions. Edit the `Claude/` copy, not the parent one, when doing RE work — but be aware it can drift from the buildable source, so verify it still assembles to the same machine code (see below) rather than assuming comment/rename-only edits are always inert. The two ROMs communicate over a shared inter-MCU DMA buffer at a fixed address offset (CPU1_addr = CPU2_addr + `0xDA`, confirmed via cross-named variable pairs, e.g. `dmarx_max_retard_23B_161` on CPU1 = `dmatx_max_retard_161` on CPU2) — when a CPU1 `dmarx_*`/CPU2 `dmatx_*` variable's purpose is unclear, check the other ROM's corresponding address for a computation that reveals it (see `docs/fuel_calculation_system.md` for a worked example).
- **The `mov` instruction's operand order is `src, dest` — the opposite of `ld`/`st`, which are `dest, src`.** E.g. `mov x, d` means `D = X`, not `X = D`. This is confirmed by the technical reference's instruction table (`mov x, d` → "D ← X") and independently by the gold-standard `knock_mcu_update.ASM`'s annotation of `mov a, b` as "B = var_knock_info" where A held that value. It is very easy to misread and has caused real mistakes in this repo's comments, including in sessions after this note was needed — pre-existing comments in the shared math library (e.g. `mov s, x` annotated "SP = X") show the same reversed-notation error and haven't been swept and fixed. When a `mov`-involving computation matters, re-derive the direction from the instruction table rather than trusting an existing comment.
- **After editing an `.ASM`/`.asm` file, verify it still assembles to an identical binary** before considering the change safe, since renames/comments should never change actual bytes but a slipped instruction easily can. Assemble both the edited file and its buildable counterpart with `python roms\d8x_assembler\asm_d8x.py <input> <out.bin> <out.lst>` (prefer this over `roms\bin\Tasm32.exe` — see "Repository layout" above), then compare the two `.lst` listings with `python roms\verify_assembly_match.py <lst1> <lst2>` — **not** a raw `.out`/`.bin` byte diff or listing text diff. That script re-anchors the comparison on each `.lst`'s address column (confirmed to parse `asm_d8x.py`'s listing format correctly, not just tasm32's), because a single real edit (e.g. one extra/missing instruction byte) shifts every downstream absolute address reference by that same amount, and a naive diff reports hundreds of spurious "differences" that are really just correctly-recomputed jump/load targets following the one real change. Exit code 0 / "Total real edit regions: 0" means the binaries match. (`roms\bin\Tasm32.exe`/`roms\bin\checksum.exe`/`roms\bin\scramble.exe` may fail to run at all in some environments with an unrelated DLL-load error — that's not a sign anything is wrong with the ASM; this comparison doesn't need them.)
