# D8X Assembly for VS Code

Language support for the Toshiba/Denso 8X (D8X) disassemblies under `roms/`: syntax highlighting, **Go to Definition**, **Find All References**, hover, outline and occurrence highlighting.

The syntax follows what `roms/d8x_assembler/asm_d8x.py` accepts, not a guess at TASM. A label is whatever starts at column 0, with or without a colon. The first word after it is the mnemonic or directive. Labels are case-sensitive; mnemonics and registers are not.

## Install

```
python tools/vscode-d8x/package_vsix.py --install
```

Then run **Developer: Reload Window**. This builds `d8x-asm-<version>.vsix` without node or `vsce`, and installs it with `code --install-extension`. Re-run it after changing anything here.

The packager first checks the grammar's mnemonic lists against `instruction.opcodes`. If an opcode is added to the assembler, add it to `mnemonic-flow` or `mnemonic-other` in `syntaxes/d8x.tmLanguage.json`, or packaging stops.

`.vscode/settings.json` maps `*.asm`/`*.ASM` to this language for the repo. Other installed assembly extensions that also claim `.asm` don't win here.

## What it does

| Feature | Notes |
|---|---|
| Highlighting | Mnemonics, with branches, jumps, calls and returns set apart. Registers, `bit0`–`bit7`, the on-chip registers (`PORTA`, `CPR0`, `SSD` …), numbers in every form the assembler parses (`0AA55h`, `$FF`, `0101b`, `42`), `.define`/`.equ`/`.macro`, and IDA's `CODE XREF:`/`DATA XREF:` markers. Data labels (`.block`/`.db`/`.dw`/`.equ`) are coloured apart from code labels. The last operand of a branch is coloured as a function. |
| Go to Definition (F12, Ctrl+click) | Labels, `.equ`, `.define` and `.macro`. **Also works inside comments**, including IDA xref lists: in `; CODE XREF: sub_C59B+1B5w` or `map_2d_interpolate↑p`, the access-type letter and arrow are stripped. That covers raw CP437 files too, where the arrow is byte `0x18`/`0x19`. |
| Find All References (Shift+F12) | Every use in code, plus mentions in comment prose (`d8x.references.includeComments`). IDA's xref lists are always left out: each entry only restates a code reference that is already in the results. |
| Hover | The definition line, the column-0 comment block above it (e.g. a routine's header) and the comment beside it, minus xref lists and IDA banners. |
| Outline / Ctrl+Shift+O / breadcrumbs | Every label. Each one spans the lines up to the next, so breadcrumbs follow the cursor through a routine. |
| Occurrence highlighting | All uses of the symbol under the cursor, the definition marked as a write. |

**Scope is one file.** Every `.ASM` here is a complete ROM with no `.include`. The same name in two ROMs (`-9651` and `-9661`, say) is two different things, so nothing resolves across files. For a symbol's counterpart on the other CPU, see the DMA offsets in `CLAUDE.md`.

## Tab width

IDA lays these files out with 8-column tabs: mnemonics at column 32, comments at column 64. VS Code defaults to 4, and can't detect a width from a file indented only with tabs, so the comments come out ragged. The extension makes 8 the default for this language. `roms/tidy_asm.py` puts a file back on IDA's columns after edits have moved things.

## Encoding

This extension only reads the text the editor has already decoded; it never saves. Saving is still the risk described in `.vscode/settings.json` and `CLAUDE.md`. Most `.ASM` files are CP437, and opened as UTF-8 their banner bytes show as `�` and are corrupted on save. Check the encoding in the status bar before saving.

## Dimming IDA's placeholder names

`sub_`/`loc_`/`unk_` names mean "not yet understood" in this repo. They get their own scopes, so you can dim them in your user settings:

```json
"editor.tokenColorCustomizations": {
  "textMateRules": [
    {
      "scope": ["variable.other.unnamed.d8x", "entity.name.function.unnamed.d8x"],
      "settings": { "foreground": "#808080" }
    }
  ]
}
```

## Tests

There is no node on the bench machine, so the tests run under VS Code's own Electron:

```
ELECTRON_RUN_AS_NODE=1 "$LOCALAPPDATA/Programs/Microsoft VS Code/Code.exe" tools/vscode-d8x/test.js
```

They run against the real ROM sources, including the UTF-8 `Claude/` copies, a raw CP437 `.ASM` and a DIAG16 image. They check that:

- every symbol used as an operand resolves to a definition;
- no label is defined twice;
- IDA xrefs resolve in both encodings;
- references and hovers leave out xref lists.

The grammar is checked with the `vscode-textmate` engine that ships inside VS Code: scopes for representative lines, plus a full tokenization of each ROM in which every instruction must be a known mnemonic.

## Files

- `syntaxes/d8x.tmLanguage.json` — the TextMate grammar.
- `symbols.js` — definition/reference/hover logic, with no `vscode` dependency so it can be tested directly.
- `extension.js` — the VS Code providers.
- `package_vsix.py` — the packager.
- `test.js` — the tests.
