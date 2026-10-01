"""Restore IDA's column layout in a D8X .ASM/.asm file.

    python roms/tidy_asm.py <file> [<file> ...]          # rewrite in place
    python roms/tidy_asm.py --check <file> [<file> ...]  # report only, exit 1 if untidy

IDA exports with 8-column tabs: labels at column 0, mnemonics and
directives at column 32, trailing comments at column 64 - or one space after
the code when the code runs past it - and comment continuation lines at
column 64. Renaming a label changes its length without changing the tabs
after it, and hand-added comments land wherever the editor was, so the layout
drifts. This puts every line back on those columns.

Only the whitespace between label, statement and comment changes; the text
of each is kept byte-for-byte. Still verify the result assembles to the same
listing (see CLAUDE.md, "After editing").

The file is read and written through latin1, which round-trips every byte,
so this is safe on both the raw CP437 sources and the UTF-8 Claude/ copies:
the only text it measures is code, which is ASCII. Line endings are kept.

Left as IDA wrote them:
  - column-0 comments (headers, banners);
  - comment-only lines at column 32 (IDA's '; public X', ';.segment X');
  - a comment separated by spaces alone from short code - IDA's character
    hints on data, '.db 5Fh ; _'.
"""

import argparse
import re
import sys

TAB = 8
STATEMENT_COL = 32
COMMENT_COL = 64
KEEP_COMMENT_COLS = {0, 32}

# Only spaces and tabs are layout. str.strip() would also take 0xA0 and
# 0x1C-0x1F, which in these files are CP437 glyphs - IDA's character hint
# for a 0xA0 byte, '.db 0A0h ; ' followed by that byte, for one.
WS = ' \t'

# A label ends at its colon: IDA writes a 32-character label straight into
# its directive, 'var_o2_heater_current_error_cnt:.block 1'.
LABEL_RE = re.compile(r'^([^\s;:]+:?)')


def column(text):
    col = 0
    for ch in text:
        col = (col // TAB + 1) * TAB if ch == '\t' else col + 1
    return col


def pad(col, target, short=' '):
    """Whitespace from col to target, or `short` if already at or past it."""
    if col >= target:
        return short
    tabs = (target - col + (col % TAB) + TAB - 1) // TAB
    return '\t' * tabs


def comment_start(text):
    quote = None
    for i, ch in enumerate(text):
        if quote:
            if ch == quote:
                quote = None
        elif ch in '"\'':
            quote = ch
        elif ch == ';':
            return i
    return -1


def tidy_line(line):
    text = line.rstrip(WS)
    if not text:
        return ''
    c = comment_start(text)
    code = text if c < 0 else text[:c]
    comment = '' if c < 0 else text[c:]

    if not code.strip(WS):
        if column(code) in KEEP_COMMENT_COLS:
            return text
        return pad(0, COMMENT_COL) + comment

    m = LABEL_RE.match(code)
    label = m.group(1) if m else ''
    statement = code[len(label):].strip(WS)

    out = label
    if statement:
        out += pad(column(out), STATEMENT_COL, short='\t') + statement
    if comment:
        separator = code[len(code.rstrip(WS)):]
        if statement and '\t' not in separator and column(out) < COMMENT_COL - 1:
            out += separator or ' '          # IDA's '.db 5Fh ; _' hints
        else:
            out += pad(column(out), COMMENT_COL)
        out += comment
    return out


def tidy(data):
    text = data.decode('latin1')
    eol = '\r\n' if '\r\n' in text else '\n'
    lines = text.split(eol)
    return eol.join(tidy_line(l) for l in lines).encode('latin1')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--check', action='store_true', help='report without writing')
    ap.add_argument('files', nargs='+')
    args = ap.parse_args()

    untidy = 0
    for path in args.files:
        with open(path, 'rb') as f:
            before = f.read()
        after = tidy(before)
        eol = b'\r\n' if b'\r\n' in before else b'\n'
        changed = sum(a != b for a, b in zip(before.split(eol), after.split(eol)))
        if changed:
            untidy += 1
        if args.check or not changed:
            print(f'{path}: {changed} line(s) {"to change" if args.check else "changed"}')
        else:
            with open(path, 'wb') as f:
                f.write(after)
            print(f'{path}: {changed} line(s) changed')
    sys.exit(1 if args.check and untidy else 0)


if __name__ == '__main__':
    main()
