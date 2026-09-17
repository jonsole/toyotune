#!/usr/bin/env python3
"""
Turn the dash node's console screenshot into a PNG.

    python tools/screenshot.py capture.txt shot.png
    python tools/screenshot.py --port COM5 shot.png       (needs pyserial)

The firmware answers an 'S' on its USB console by printing what the panel was
last sent from the page on the glass: a SCREENSHOT BEGIN line, the palette as
the panel receives it (RGB565, high byte first), every row as hex palette
indices, and SCREENSHOT END. Status lines can be mixed in around it; they are
ignored. Remember the port needs DTR asserted or the firmware prints nothing.

The PNG is what the panel was SENT - the back buffer - not a photograph, so it
shows exactly what the renderer drew and nothing of how the glass looks.
"""

import argparse
import sys

from PIL import Image


# A row lost in transit is drawn in this, so it cannot pass for content.
MISSING = (255, 0, 255)


def parse(lines):
    """Rows are "R <number> <hex>". Anything that does not parse - a status
    line printed into the middle, a row whose start was lost - is skipped, and
    the rows it should have been are reported rather than guessed."""
    size = None
    palette = None
    rows = {}
    inside = False
    for line in lines:
        line = line.strip()
        if line.startswith("SCREENSHOT BEGIN"):
            parts = line.split()
            size = (int(parts[2]), int(parts[3]))
            palette, rows, inside = None, {}, True
        elif not inside:
            continue
        elif line.startswith("PALETTE "):
            h = line[8:]
            palette = [int(h[i:i + 4], 16) for i in range(0, len(h), 4)]
        elif line.startswith("R "):
            parts = line.split()
            if len(parts) == 3 and parts[1].isdigit() and len(parts[2]) == 2 * size[0]:
                try:
                    rows[int(parts[1])] = bytes.fromhex(parts[2])
                except ValueError:
                    pass
        elif line.startswith("SCREENSHOT END"):
            inside = False
            if size and palette:
                return size, palette, rows
    sys.exit("no complete screenshot found")


def to_png(size, palette, rows, out):
    rgb = []
    for v in palette:
        r, g, b = (v >> 11) & 31, (v >> 5) & 63, v & 31
        rgb.append((r * 255 // 31, g * 255 // 63, b * 255 // 31))
    img = Image.new("RGB", size, MISSING)
    px = img.load()
    for y, row in rows.items():
        if 0 <= y < size[1]:
            for x, n in enumerate(row[:size[0]]):
                px[x, y] = rgb[n]
    img.save(out)
    return [y for y in range(size[1]) if y not in rows]


def capture(port, timeout):
    import serial  # optional; only needed for --port
    with serial.Serial(port, 115200, timeout=timeout) as s:
        s.dtr = True
        s.rts = True
        s.reset_input_buffer()
        s.write(b"S")
        lines = []
        while True:
            line = s.readline().decode("ascii", "replace")
            if not line:
                break
            lines.append(line)
            if line.startswith("SCREENSHOT END"):
                break
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("source", nargs="?", help="a captured console log")
    ap.add_argument("out", help="the PNG to write")
    ap.add_argument("--port", help="read it straight from the node (pyserial)")
    ap.add_argument("--timeout", type=float, default=5.0)
    args = ap.parse_args()

    if args.port:
        lines = capture(args.port, args.timeout)
    elif args.source:
        with open(args.source, encoding="ascii", errors="replace") as f:
            lines = f.readlines()
    else:
        sys.exit("give a captured log or --port")

    size, palette, rows = parse(lines)
    missing = to_png(size, palette, rows, args.out)
    print("wrote %s (%dx%d)" % (args.out, size[0], size[1]))
    if missing:
        print("%d row(s) lost in transit, drawn magenta: %s"
              % (len(missing), ", ".join(str(y) for y in missing[:20])))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
