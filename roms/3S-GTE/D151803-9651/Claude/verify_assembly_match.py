#!/usr/bin/env python3
"""
Compare two tasm32 .lst listing files by address, not by raw text/hex diff.

Usage: verify_assembly_match.py <orig.lst> <claude.lst>

Why not just `fc /b` the two .out files or diff the .lst text directly?
tasm32's .out format has a small non-ROM header, and any single real
byte-count difference (e.g. one extra/missing instruction) shifts every
absolute address reference for the rest of the file, which then makes the
assembler correctly re-encode every downstream jump/load target that changed
by that same shift. A naive diff reports hundreds of "differences" that are
all just consequences of one real edit. This script re-anchors on the
address column of the .lst file instead, so a single genuine edit shows up
as exactly one edit region, and everything downstream that merely shifted
in address is correctly treated as unchanged.

See docs/session_journal.md for the incident this was built to diagnose:
a stray extra `ret` in divide_rD_2_saturate that broke an intentional
fall-through into clamp_rD_FF.
"""
import re
import sys
import difflib


def parse(path):
    """Return {address: [hex_byte_str, ...]} from a tasm32 .lst file."""
    addr_bytes = {}
    with open(path, 'r', encoding='latin-1') as f:
        for raw in f:
            line = raw.rstrip('\n').rstrip('\r')
            if len(line) < 20:
                continue
            lineno_field = line[0:7]
            addr_field = line[7:11]
            rest = line[11:]
            if not re.match(r'^\d+$', lineno_field.strip()):
                continue
            if not re.match(r'^[0-9A-Fa-f]{4}$', addr_field.strip()):
                continue
            bytes_field = rest[:13]
            hexbytes = re.findall(r'[0-9A-Fa-f]{2}', bytes_field)
            if not hexbytes:
                continue
            addr_bytes[int(addr_field, 16)] = hexbytes
    return addr_bytes


def flatten(addr_bytes, lo, hi):
    arr = ['--'] * (hi - lo + 1)
    for addr, hexlist in addr_bytes.items():
        for i, hx in enumerate(hexlist):
            idx = addr + i - lo
            if 0 <= idx < len(arr):
                arr[idx] = hx
    return arr


def main():
    a = parse(sys.argv[1])
    b = parse(sys.argv[2])

    min_addr = min(min(a.keys()), min(b.keys()))
    max_addr_a = max(addr + len(v) - 1 for addr, v in a.items())
    max_addr_b = max(addr + len(v) - 1 for addr, v in b.items())
    max_addr = max(max_addr_a, max_addr_b)

    fa = flatten(a, min_addr, max_addr)
    fb = flatten(b, min_addr, max_addr)

    sm = difflib.SequenceMatcher(None, fa, fb, autojunk=False)
    print(f"file 1 range: {min_addr:04X}-{max_addr_a:04X} ({len(fa)} bytes)")
    print(f"file 2 range: {min_addr:04X}-{max_addr_b:04X} ({len(fb)} bytes)")
    print()
    count = 0
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == 'equal':
            continue
        count += 1
        addr1 = min_addr + i1
        addr2 = min_addr + j1
        bytes1 = ' '.join(fa[i1:i2])
        bytes2 = ' '.join(fb[j1:j2])
        print(f"[{tag}] file1 @ {addr1:04X} ({i2 - i1} bytes): {bytes1}")
        print(f"         file2 @ {addr2:04X} ({j2 - j1} bytes): {bytes2}")
        print()
    print(f"Total real edit regions: {count}")
    return 1 if count else 0


if __name__ == '__main__':
    sys.exit(main())
