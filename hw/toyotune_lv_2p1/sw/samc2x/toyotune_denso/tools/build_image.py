#!/usr/bin/env python3
"""Build a Denso ROM and emit it as image.c for the SAMC21 firmware.

The v2.1 board has no EPROM: the SAMC21 writes a 32K image into the SRAM the
Denso MCU executes from (main.c, XMEM_BlockWrite(0x8000, DMCU_Image, 32768)),
then releases reset.  That image is compiled into the firmware as the
DMCU_Image[] array in image.c, so changing the ROM the ECU runs means
regenerating that array.  This script does the whole chain:

    .asm  --assemble-->  raw  --pad-->  32K image  --checksum-->  image.c

and it reproduces the committed image.c byte-for-byte from
roms/3S-GTE/D151803-9651/D151803-9651.ASM, which is what --verify checks.

Why the checksum and padding are reimplemented here
---------------------------------------------------
The ROM Makefiles shell out to roms/bin/checksum.exe and roms/bin/scramble.exe.
Both are MSVC builds that fail with STATUS_DLL_NOT_FOUND on machines without
the matching redistributable, which would make this script unrunnable on
exactly the machines most likely to need it.  Both steps are a few lines of
arithmetic, so they are ported here instead:

  * checksum() is a port of roms/checksum/checksum.cpp - same OMODE fix, same
    16-bit word sum, same adjustment word at 0xFFDA/0xFFDB.  It reproduces the
    exact two-byte difference (0xC604 and 0xFFDA) that checksum.exe left in
    the committed image.  It differs from the .exe in one deliberate way; see
    CHECKSUM_START below.
  * padding replicates the Makefile's "scramble ... 8000 FF 00 01234567" call,
    which with an identity code and a zero XOR is only "place the ROM at the
    top of a 32K buffer of 0xFF".

The assembler itself is NOT reimplemented - roms/d8x_assembler/asm_d8x.py is
invoked, with -p 5F to match the fill byte the Makefiles use.

Usage
-----
    # regenerate image.c from a ROM source
    python tools/build_image.py <asm> -o image.c

    # check the committed image.c still matches its source
    python tools/build_image.py <asm> --verify image.c

    # convert an already-built .bin instead of assembling
    python tools/build_image.py some_rom.bin -o image.c
"""

import argparse
import datetime
import hashlib
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# tools/ -> toyotune_denso/ -> samc2x/ -> sw/ -> toyotune_lv_2p1/ -> hw/ -> repo root
REPO_ROOT = os.path.abspath(os.path.join(HERE, *([os.pardir] * 6)))
ASSEMBLER = os.path.join(REPO_ROOT, "roms", "d8x_assembler", "asm_d8x.py")

# Matches ASMFLAGS in roms/<family>/makefile.lib - Tasm32.exe's old -f5F.
FILL_BYTE = 0x5F

DEFAULT_IMAGE_SIZE = 32768
DEFAULT_PAD_BYTE = 0xFF
DEFAULT_ARRAY = "DMCU_Image"
BYTES_PER_LINE = 12

# Where the ECU's own power-on ROM test starts summing.
#
# This is NOT a property of the image size, and it is NOT the same in every
# ROM - so it is read back out of the built image rather than assumed.  The
# self test is:
#
#     ld  x, #<start>         ; 8E hi lo
#     clr a / clr b           ; 52 53
#     ld  y, #0100h           ; 8F 01 00   - 256 words per outer pass
#     add d, x + 00h          ; A7 00
#     inc x / inc x / dec y   ; 1C 1C 1F
#     bne -7                  ; 46 F9
#     ...
#     cmp x, #0000h           ; run until X wraps back to 0000
#     cmp d, #0AA55h
#
# The operand of that first "ld x" is the answer, and it varies:
#
#     D151803-9651, -9661, -0461, -0471, -0481, 3VZ 0680   C000  (16K ROMs)
#     D151803-9651_DIAG16_32K                              C000  (32K image!)
#     D151804-0461_DIAG16_32K_JS                           8000
#     Jon_ST205_ECU/D151804-0461, -0471                    8000
#
# So the 32K sources disagree with each other: the 0461 DIAG16 mod extends the
# self test down over its added diagnostic code at 0x8000, while the 9651
# DIAG16 mod leaves it covering only the top 16K.  Hardcoding either value
# silently produces an adjustment word that fails the self test on the ROMs
# using the other one, which is why CHECKSUM_SIGNATURE exists.
#
# checksum.exe gets this wrong in a third way: it infers the start from the
# input file's *length*.  That gives C000 for a 16K ROM but 8000 for any 32K
# source, which is right for the 0461 DIAG16 only by coincidence and wrong for
# the 9651 one.
CHECKSUM_SIGNATURE = re.compile(
    rb"\x8e(..)\x52\x53\x8f\x01\x00\xa7\x00\x1c\x1c\x1f\x46\xf9", re.S)

# Used only when the signature is absent (a ROM whose self test was removed or
# rewritten), and always with a warning.
CHECKSUM_START_FALLBACK = 0xC000
CHECKSUM_TARGET = 0xAA55
CHECKSUM_ADJ_ADDR = 0xFFDA


def detect_checksum_start(image):
    """Return the address the image's own ROM self test starts summing from.

    Returns None if the self-test loop is not found.  Exits if the image
    contains more than one loop disagreeing about the start address, since
    guessing which one the ECU runs would be worse than stopping.
    """
    found = sorted({(m.group(1)[0] << 8) | m.group(1)[1]
                    for m in CHECKSUM_SIGNATURE.finditer(image)})
    if not found:
        return None
    if len(found) > 1:
        sys.exit("image contains %d ROM self tests with different start "
                 "addresses (%s); pass --checksum-start to say which applies"
                 % (len(found), ", ".join("%04X" % a for a in found)))
    return found[0]


def assemble(asm_path, listing_path=None):
    """Run asm_d8x.py over asm_path and return the raw output bytes."""
    if not os.path.exists(ASSEMBLER):
        sys.exit("assembler not found: %s" % ASSEMBLER)

    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "rom.out")
        cmd = [sys.executable, ASSEMBLER, "-p", "%02X" % FILL_BYTE, asm_path, out]
        if listing_path:
            cmd.append(listing_path)
        proc = subprocess.run(cmd, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            sys.stderr.write(proc.stdout or "")
            sys.exit("assembly of %s failed (exit %d)" % (asm_path, proc.returncode))
        with open(out, "rb") as f:
            return f.read()


def checksum(image, start):
    """Port of roms/checksum/checksum.cpp, applied to the finished image.

    The image occupies the TOP of the Denso's 64K address space, so a 32K
    image sits at 0x8000..0xFFFF.  Two things happen to it:

      1. The reset vector at 0xFFFE/0xFFFF is followed to the first
         instruction; if that is "ld #07h,OMODE" (33 07) the operand is
         patched to 02.
      2. The 16-bit words from `start` to 0xFFFF are summed and an adjustment
         word is stored at 0xFFDA/0xFFDB so the total comes to 0xAA55, which
         is what the ECU's own power-on ROM test checks.

    Returns (patched_image, adjustment, omode_addr_or_None).
    """
    if len(image) % 2:
        sys.exit("image length %d is odd; the checksum sums 16-bit words"
                 % len(image))
    if len(image) > 65536:
        sys.exit("image length %d exceeds the 64K address space" % len(image))

    base = 65536 - len(image)
    if start < base:
        sys.exit("checksum start %04X is below the image, which begins at %04X"
                 % (start, base))
    if start % 2:
        sys.exit("checksum start %04X is odd; the sum walks 16-bit words" % start)

    mem = bytearray(65536)
    mem[base:] = image

    omode = None
    vector = (mem[0xFFFE] << 8) | mem[0xFFFF]
    # checksum.cpp reads mem[vector] out of an uninitialised malloc buffer when
    # the vector points below the loaded image.  Warn rather than patch garbage.
    if vector < base:
        sys.stderr.write("warning: reset vector %04X points below the image at "
                         "%04X; skipping the OMODE patch\n" % (vector, base))
    elif mem[vector] == 0x33 and mem[vector + 1] == 0x07:
        mem[vector + 1] = 0x02
        omode = vector

    mem[CHECKSUM_ADJ_ADDR] = 0
    mem[CHECKSUM_ADJ_ADDR + 1] = 0
    total = 0
    for addr in range(start, 65536, 2):
        total = (total + ((mem[addr] << 8) | mem[addr + 1])) & 0xFFFF

    adjustment = (CHECKSUM_TARGET - total) & 0xFFFF
    mem[CHECKSUM_ADJ_ADDR] = (adjustment >> 8) & 0xFF
    mem[CHECKSUM_ADJ_ADDR + 1] = adjustment & 0xFF
    return bytes(mem[base:]), adjustment, omode


def pad(rom, size, fill):
    """Place the ROM at the top of a `size` buffer of `fill`.

    Equivalent to the Makefile's rom_toyotune step.  The Denso executes from
    the top of its address space, and the firmware writes this whole buffer to
    SRAM at 0x8000 - so a 16K ROM in a 32K image lands at 0xC000..0xFFFF.
    """
    if len(rom) > size:
        sys.exit("ROM is %d bytes, larger than the %d-byte image" % (len(rom), size))
    return bytes([fill]) * (size - len(rom)) + rom


def emit_c(image, rom_len, source_path, array_name, pad_byte):
    """Render the image as image.c, in the format of the committed file."""
    try:
        rel = os.path.relpath(source_path, REPO_ROOT).replace(os.sep, "/")
    except ValueError:                      # different drive on Windows
        rel = source_path
    # The source file's own mtime, not the time of generation, so that
    # regenerating an unchanged source produces an unchanged file.
    mtime = datetime.datetime.fromtimestamp(
        os.path.getmtime(source_path)).strftime("%d/%m/%Y %H:%M:%S")

    lines = [
        "/*",
        " * image.c",
        " *",
        " * The ROM image the SAMC21 writes into the Denso MCU's SRAM at boot.",
        " *",
        " * GENERATED FILE - do not edit by hand.  Regenerate with:",
        " *     python tools/build_image.py <source.asm> -o image.c",
        " * and check it still matches its source with:",
        " *     python tools/build_image.py <source.asm> --verify image.c",
        " */",
        "",
        "#include <stdint.h>",
        "",
    ]
    if rom_len != len(image):
        lines.append("/* A %d-byte ROM; the low %d bytes are 0x%02X padding, so once"
                     % (rom_len, len(image) - rom_len, pad_byte))
        lines.append("   written to SRAM at 0x8000 the ROM lands at %04X..FFFF. */"
                     % (65536 - rom_len))
    lines += [
        "/* %s (%s)" % (rel, mtime),
        "   StartOffset(h): 00000000, EndOffset(h): %08X, Length(h): %08X"
        % (len(image) - 1, len(image)),
        "   sha256: %s */" % hashlib.sha256(image).hexdigest(),
        "",
        "const uint8_t /*__attribute__((section (\".mcu_image\")))*/ %s[%d] ="
        % (array_name, len(image)),
        "{",
    ]
    for i in range(0, len(image), BYTES_PER_LINE):
        row = ", ".join("0x%02X" % b for b in image[i:i + BYTES_PER_LINE])
        lines.append("\t" + row + ("," if i + BYTES_PER_LINE < len(image) else ""))
    lines += ["};", ""]
    return "\n".join(lines)


def parse_c_array(path, array_name):
    """Pull the bytes of `array_name` back out of a .c file.

    Regions inside "#if 0" are stripped first, so a disabled second array -
    which the committed image.c carries - is never picked up by mistake.
    """
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()

    kept, depth, disabled_at = [], 0, None
    for line in text.split("\n"):
        stripped = line.strip()
        if stripped.startswith("#if"):
            depth += 1
            if disabled_at is None and re.match(r"#if\s+0\s*$", stripped):
                disabled_at = depth
        elif stripped.startswith("#endif"):
            if disabled_at is not None and depth == disabled_at:
                disabled_at = None
                depth = max(0, depth - 1)
                continue
            depth = max(0, depth - 1)
        if disabled_at is None:
            kept.append(line)
    text = "\n".join(kept)

    m = re.search(r"\b%s\s*\[[^\]]*\]\s*=\s*\{(.*?)\}\s*;" % re.escape(array_name),
                  text, re.S)
    if not m:
        sys.exit("no enabled '%s[]' definition found in %s" % (array_name, path))
    return bytes(int(h, 16) for h in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))


def main():
    ap = argparse.ArgumentParser(
        description="Build a Denso ROM and emit it as the SAMC21 firmware's image.c")
    ap.add_argument("source",
                    help=".asm/.ASM source to assemble, or a .bin/.BIN to convert")
    ap.add_argument("-o", "--output", metavar="FILE",
                    help="write image.c here (default: stdout)")
    ap.add_argument("--verify", metavar="FILE",
                    help="compare against the array in this existing .c instead "
                         "of writing; exit 1 if it differs")
    ap.add_argument("--array-name", default=DEFAULT_ARRAY,
                    help="C array name (default: %(default)s)")
    ap.add_argument("--size", type=lambda s: int(s, 0), default=DEFAULT_IMAGE_SIZE,
                    help="image size in bytes (default: 32768)")
    ap.add_argument("--pad-byte", type=lambda s: int(s, 0), default=DEFAULT_PAD_BYTE,
                    help="fill byte below the ROM (default: 0xFF)")
    ap.add_argument("--no-checksum", action="store_true",
                    help="skip the OMODE patch and checksum adjustment")
    ap.add_argument("--checksum-start", type=lambda s: int(s, 0),
                    default=None, metavar="ADDR",
                    help="Denso address the ECU's ROM self test starts summing "
                         "from. By default this is read out of the image's own "
                         "self-test loop, which is the only reliable source - "
                         "the 32K ROMs here use C000 and 8000 inconsistently.")
    ap.add_argument("--bin", metavar="FILE",
                    help="also write the padded image as a raw .bin")
    ap.add_argument("--listing", metavar="FILE",
                    help="also write the assembler .lst listing")
    args = ap.parse_args()

    if not os.path.exists(args.source):
        sys.exit("no such file: %s" % args.source)

    ext = os.path.splitext(args.source)[1].lower()
    if ext == ".asm":
        rom = assemble(args.source, args.listing)
    elif ext == ".bin":
        if args.listing:
            sys.exit("--listing only applies when assembling a .asm source")
        with open(args.source, "rb") as f:
            rom = f.read()
    else:
        sys.exit("unrecognised source type '%s'; expected .asm or .bin" % ext)

    rom_len = len(rom)
    image = pad(rom, args.size, args.pad_byte)

    sys.stderr.write("source %d bytes at %04X..FFFF, image %d bytes at %04X..FFFF\n"
                     % (rom_len, 65536 - rom_len, len(image), 65536 - len(image)))

    adjustment = omode = None
    if not args.no_checksum:
        # Read the summed range out of the image's own self test rather than
        # assuming it - the 32K ROMs here do not agree on it.
        start = args.checksum_start
        if start is None:
            start = detect_checksum_start(image)
            if start is None:
                start = CHECKSUM_START_FALLBACK
                sys.stderr.write(
                    "warning: no ROM self-test loop found in this image, so the "
                    "summed range could not be read from it; assuming %04X. If "
                    "this ROM checks a different range the adjustment word will "
                    "be wrong - pass --checksum-start.\n" % start)
            else:
                sys.stderr.write("  self test sums from %04X (read from the image)\n"
                                 % start)
        else:
            sys.stderr.write("  self test sums from %04X (given on the command line)\n"
                             % start)
        image, adjustment, omode = checksum(image, start)

    if omode is not None:
        sys.stderr.write("  OMODE operand patched to 02 at %04X\n" % omode)
    if adjustment is not None:
        sys.stderr.write("  checksum over %04X..FFFF: adjustment %04X at %04X\n"
                         % (start, adjustment, CHECKSUM_ADJ_ADDR))
    sys.stderr.write("  sha256 %s\n" % hashlib.sha256(image).hexdigest())

    if args.verify:
        existing = parse_c_array(args.verify, args.array_name)
        if existing == image:
            sys.stderr.write("OK: %s matches %s\n" % (args.verify, args.source))
            return 0
        sys.stderr.write("MISMATCH: %s does not match %s\n"
                         % (args.verify, args.source))
        if len(existing) != len(image):
            sys.stderr.write("  length %d vs %d\n" % (len(existing), len(image)))
        else:
            diffs = [i for i in range(len(image)) if existing[i] != image[i]]
            sys.stderr.write("  %d differing bytes; first at image offset %04X "
                             "(Denso %04X): %02X in the .c vs %02X built\n"
                             % (len(diffs), diffs[0],
                                65536 - len(image) + diffs[0],
                                existing[diffs[0]], image[diffs[0]]))
        return 1

    text = emit_c(image, rom_len, args.source, args.array_name, args.pad_byte)

    if args.bin:
        with open(args.bin, "wb") as f:
            f.write(image)
        sys.stderr.write("wrote %s\n" % args.bin)

    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        sys.stderr.write("wrote %s\n" % args.output)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
