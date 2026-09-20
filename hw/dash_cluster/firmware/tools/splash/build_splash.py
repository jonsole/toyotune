#!/usr/bin/env python3
"""
The power-on splash, from the pictures in hw/dash_cluster/art.

    python tools/splash/build_splash.py --preview    PNGs to look at, nothing written
    python tools/splash/build_splash.py              writes src/dash_splash.c

Four full-screen images: the Toyota emblem, and the three letters of the MR2
badge, one per node. Each is cut out of its photograph, scaled to the 466x466
panel and reduced to 8-bit indices into a palette of its own - the splash is
the only thing on the glass while it runs, so it does not have to share the
gauges' palette, and the chrome can have every grey it needs.

THE EMBLEM is cut from toyota_logo_dark.png, chrome on a carbon-fibre weave,
by texture rather than colour - see emblem() for why neither of the obvious
tests works on either picture.

THE LETTERS are cut from mr2_logo.png, white letters on the black badge,
about 125 px tall there and about 200 on the glass. (They were first taken
from mr2_badge.jpg's red lettering, 70 px tall, and scaling that nearly four
times turned the JPEG's noise into ripples along the M.) Each letter's shape
is still smoothed at the picture's own resolution before it is scaled - see
letters(). They are drawn in the gauges' own off-white (UI_GAUGE_MARK_COLOUR)
on black: the badge supplies the lettering, not its colour.
"""

import argparse
import os
import sys

import numpy as np
from scipy import ndimage
from PIL import Image, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.normpath(os.path.join(HERE, "..", ".."))
ART = os.path.normpath(os.path.join(FIRMWARE, "..", "art"))
BUILD = os.path.join(HERE, "build")

# Hand-cleaned letters. A file here named letter_<X>_4x.png - white on black,
# four times the panel's size - replaces the shape traced from the badge for
# that letter. --export-letters writes the traced ones here to start from.
LETTERS_DIR = os.path.join(ART, "letters")
LETTER_NAMES = ("M", "R", "2")

PANEL = 466

# How much of the panel each takes. The panel is round, so a wide emblem is
# limited by its corners reaching the edge, not by its width alone.
EMBLEM_WIDTH = 400
EMBLEM_SMOOTH = 6.0	# texture below this is metal - the weave is 8-10
EMBLEM_BRIGHT = 100	# and anything this bright joined to it
LETTER_FIT_RADIUS = 190		# a letter's corners stay inside this circle
LETTER_COLOUR = (0xF2, 0xEE, 0xE4)	# UI_GAUGE_MARK_COLOUR in src/ui_gauge.h
LETTER_LEVELS = 16			# black to white, the letters' antialiasing


# ---------------------------------------------------------------------------
# The emblem
# ---------------------------------------------------------------------------

def label_regions(mask):
    """Connected regions of a boolean mask, 4-connected. Small images only -
    a plain flood fill is fast enough and needs nothing beyond numpy."""
    h, w = mask.shape
    labels = np.zeros((h, w), dtype=np.int32)
    n = 0
    for y0 in range(h):
        for x0 in range(w):
            if not mask[y0, x0] or labels[y0, x0]:
                continue
            n += 1
            stack = [(y0, x0)]
            labels[y0, x0] = n
            while stack:
                y, x = stack.pop()
                for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    yy, xx = y + dy, x + dx
                    if 0 <= yy < h and 0 <= xx < w and mask[yy, xx] and not labels[yy, xx]:
                        labels[yy, xx] = n
                        stack.append((yy, xx))
    return labels, n


EMBLEM_EDITED = os.path.join(ART, "toyota_emblem.png")


def emblem_edited():
    """A hand-edited emblem, used exactly as it is: transparent background
    preferred, black accepted. Nothing is cut; it is only put on black,
    trimmed and scaled."""
    im = Image.open(EMBLEM_EDITED).convert("RGBA")
    a = np.asarray(im).astype(np.float32)
    rgb = a[..., :3] * (a[..., 3:4] / 255.0)
    img = Image.fromarray(np.clip(rgb, 0, 255).astype(np.uint8))
    ys, xs = np.nonzero(rgb.max(axis=2) > 8)
    img = img.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
    scale = EMBLEM_WIDTH / img.width
    img = img.resize((EMBLEM_WIDTH, round(img.height * scale)), Image.LANCZOS)
    print("emblem: from %s" % os.path.relpath(EMBLEM_EDITED, FIRMWARE))
    return centred(img)


def emblem():
    """The emblem, from toyota_logo_dark.png: chrome on a carbon-fibre weave.

    It was first cut from toyota_logo.jpg, chrome on white, and that cannot be
    done by colour: the chrome's highlights are exactly the background's 255
    white where they meet the rings' edges, so any white test takes pieces of
    the metal with it.

    Here brightness is not enough either - the shaded side of the metal is as
    dark as the weave - but TEXTURE is: the weave is a fine regular check, the
    metal smooth however dark. So the metal is what is smooth, plus whatever
    is bright and joined to it (the thinnest arcs are 6-8 px, about the size of
    the texture measure itself, and would otherwise be lost in its blur)."""
    if os.path.exists(EMBLEM_EDITED):
        return emblem_edited()

    src = Image.open(os.path.join(ART, "toyota_logo_dark.png")).convert("RGB")
    a = np.asarray(src).astype(np.float32)
    lum = a.mean(axis=2)

    # How much each small patch varies: high on the weave, low on metal.
    detail = ndimage.gaussian_filter(np.abs(lum - ndimage.gaussian_filter(lum, 1.5)), 3.0)

    def largest(mask):
        lab, n = ndimage.label(mask)
        if n == 0:
            return mask
        sizes = ndimage.sum(mask, lab, range(1, n + 1))
        return lab == 1 + int(np.argmax(sizes))

    metal = largest(detail < EMBLEM_SMOOTH)
    metal = largest(metal | (lum > EMBLEM_BRIGHT))

    # Specks of shadow inside the metal are metal; the spaces the rings
    # enclose are thousands of pixels and stay background.
    bg = ~metal
    lab, n = ndimage.label(bg)
    sizes = ndimage.sum(bg, lab, range(1, n + 1))
    metal |= np.isin(lab, [i + 1 for i, sz in enumerate(sizes) if sz < 600])

    # Back out over the few pixels of edge the texture measure blurred, but
    # only onto pixels bright enough to be the metal's rim.
    for _ in range(3):
        metal |= ndimage.binary_dilation(metal) & (lum > 60)

    # A smooth outline: round off the weave's check where it clings to an
    # edge, then a soft edge rather than a hard one.
    smooth = ndimage.gaussian_filter(metal.astype(np.float32), 1.2) > 0.5
    alpha = ndimage.gaussian_filter(smooth.astype(np.float32), 0.7)

    img = Image.fromarray(np.clip(a * alpha[..., None], 0, 255).astype(np.uint8))

    ys, xs = np.nonzero(alpha > 0.05)
    rgba = np.dstack([a, alpha * 255.0])[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
    Image.fromarray(np.clip(rgba, 0, 255).astype(np.uint8), "RGBA").save(
        os.path.join(ART, "toyota_emblem_cut.png"))
    img = img.crop((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
    scale = EMBLEM_WIDTH / img.width
    img = img.resize((EMBLEM_WIDTH, round(img.height * scale)), Image.LANCZOS)
    return centred(img)


# ---------------------------------------------------------------------------
# The letters
# ---------------------------------------------------------------------------

def letters():
    src = Image.open(os.path.join(ART, "mr2_logo.png")).convert("RGB")
    a = np.asarray(src).astype(np.float32)

    # How much letter, 0..1, from brightness: the badge is about 30, the
    # letters about 250, and the edge between them is the letter's own
    # antialiasing - kept soft so it survives the scaling below.
    lum = a.mean(axis=2)
    coverage = np.clip((lum - 40.0) / 200.0, 0.0, 1.0)
    solid = coverage > 0.5

    # The gauges' marks, so the splash and the dials are one family - and not
    # pure white, which is harsh on an AMOLED and the worst case for burn-in.
    colour = LETTER_COLOUR

    labels, n = label_regions(solid)
    boxes = []
    for i in range(1, n + 1):
        ys, xs = np.nonzero(labels == i)
        if len(ys) > 200:                          # letters, not specks
            boxes.append((xs.min(), ys.min(), xs.max() + 1, ys.max() + 1))
    boxes.sort()                                    # left to right: M, R, 2
    if len(boxes) != 3:
        sys.exit("expected 3 letters in the badge, found %d" % len(boxes))

    out = []
    masks4 = []
    for name, box in zip(LETTER_NAMES, boxes):
        x0, y0, x1, y1 = box
        pad = 4
        crop = coverage[y0 - pad:y1 + pad, x0 - pad:x1 + pad]

        # As large as the round panel allows: the letter's corners, not its
        # height, are what meet the edge - an M is wider than it is tall.
        scale = (2.0 * LETTER_FIT_RADIUS) / float(np.hypot(x1 - x0, y1 - y0))

        # Smooth at the photograph's own resolution, where the JPEG's noise is
        # a pixel wide, then up as a smooth field - so a straight stroke stays
        # straight instead of scaling the noise into a ripple. Thresholded at
        # four times the panel's resolution and averaged back down, which is
        # the antialiasing.
        soft = ndimage.gaussian_filter(crop, 0.7)
        big = ndimage.zoom(soft, scale * 4, order=3)
        solid4 = (big > 0.5).astype(np.float32)

        cleaned = os.path.join(LETTERS_DIR, "letter_%s_4x.png" % name)
        if os.path.exists(cleaned):
            solid4 = (np.asarray(Image.open(cleaned).convert("L")) > 127).astype(np.float32)
            print("letter %s: from %s" % (name, os.path.relpath(cleaned, FIRMWARE)))
        masks4.append(solid4)

        hh, ww = (solid4.shape[0] // 4) * 4, (solid4.shape[1] // 4) * 4
        v = solid4[:hh, :ww].reshape(hh // 4, 4, ww // 4, 4).mean(axis=(1, 3))

        rgb = np.zeros(v.shape + (3,), dtype=np.float32)
        for c in range(3):
            rgb[..., c] = v * colour[c]
        out.append(centred(Image.fromarray(rgb.astype(np.uint8))))
    return out, colour, masks4


# ---------------------------------------------------------------------------

def centred(img):
    canvas = Image.new("RGB", (PANEL, PANEL), (0, 0, 0))
    canvas.paste(img, ((PANEL - img.width) // 2, (PANEL - img.height) // 2))
    return canvas


def glass(img):
    """What the round panel shows: the square image with its corners gone."""
    yy, xx = np.mgrid[0:PANEL, 0:PANEL]
    inside = (xx - PANEL / 2 + 0.5) ** 2 + (yy - PANEL / 2 + 0.5) ** 2 <= (PANEL / 2) ** 2
    a = np.asarray(img).copy()
    a[~inside] = (40, 40, 40)
    return Image.fromarray(a)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", action="store_true", help="write PNGs only")
    ap.add_argument("--export-letters", action="store_true",
                    help="write each letter to art/letters, at 4x as the shape to edit "
                         "and at panel size as it is shown, then stop")
    args = ap.parse_args()

    os.makedirs(BUILD, exist_ok=True)
    frames = [("emblem", emblem())]
    lets, colour, masks4 = letters()
    frames += list(zip(("M", "R", "2"), lets))

    sheet = Image.new("RGB", (PANEL * len(frames) + 16 * (len(frames) - 1), PANEL), (255, 255, 255))
    for i, (name, img) in enumerate(frames):
        img.save(os.path.join(BUILD, "splash_%s.png" % name))
        sheet.paste(glass(img), (i * (PANEL + 16), 0))
    sheet.save(os.path.join(BUILD, "splash_sheet.png"))
    print("wrote", os.path.join(BUILD, "splash_sheet.png"))

    if args.export_letters:
        os.makedirs(LETTERS_DIR, exist_ok=True)
        for name, mask, (_, shown) in zip(LETTER_NAMES, masks4, frames[1:]):
            big = os.path.join(LETTERS_DIR, "letter_%s_4x.png" % name)
            Image.fromarray((mask * 255).astype(np.uint8)).save(big)
            x, y, crop = crop_even(shown)
            crop.convert("L").save(os.path.join(LETTERS_DIR, "letter_%s.png" % name))
            print("wrote letter_%s_4x.png (%dx%d) and letter_%s.png (%dx%d)"
                  % (name, mask.shape[1], mask.shape[0], name, crop.width, crop.height))
        return

    if args.preview:
        return

    images = [quantise_emblem(frames[0][1])]
    images += [quantise_letter(img) for _, img in frames[1:]]
    write_c(images)


# ---------------------------------------------------------------------------
# Into the firmware
# ---------------------------------------------------------------------------

def crop_even(img):
    """The non-black part, widened to even edges: the panel addresses its
    window in 2-pixel units and rounds an odd edge itself, so an odd rectangle
    would be sent a pixel out."""
    a = np.asarray(img)
    ys, xs = np.nonzero(a.max(axis=2) > 0)
    x0, y0 = (xs.min() // 2) * 2, (ys.min() // 2) * 2
    x1, y1 = ((xs.max() + 2) // 2) * 2, ((ys.max() + 2) // 2) * 2
    return x0, y0, img.crop((x0, y0, x1, y1))


def quantise_emblem(img):
    """The chrome: up to 255 greys, chosen from the picture, with exact black
    as index 0 - so the cleared screen and the background are the same
    colour and index 0 fades to itself."""
    x, y, crop = crop_even(img)
    q = crop.quantize(colors=255, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE)
    pal = np.array(q.getpalette()[:255 * 3], dtype=np.int32).reshape(-1, 3)
    idx = np.asarray(q).astype(np.int32)

    # Move the darkest entry to 0 and make it exact black.
    dark = int(np.argmin(pal.sum(axis=1)))
    order = [dark] + [i for i in range(len(pal)) if i != dark]
    remap = np.zeros(len(pal), dtype=np.int32)
    for new, old in enumerate(order):
        remap[old] = new
    pal = pal[order]
    pal[0] = (0, 0, 0)
    used = int(idx.max()) + 1
    return {"x": x, "y": y, "w": crop.width, "h": crop.height,
            "data": remap[idx].astype(np.uint8), "palette": pal[:max(used, 1)]}


def quantise_letter(img):
    """A letter: its coverage in LETTER_LEVELS steps, black to off-white."""
    x, y, crop = crop_even(img)
    a = np.asarray(crop).astype(np.float32)
    cover = a.max(axis=2) / float(max(LETTER_COLOUR))
    idx = np.clip(np.round(cover * (LETTER_LEVELS - 1)), 0, LETTER_LEVELS - 1).astype(np.uint8)
    pal = np.array([[round(c * k / (LETTER_LEVELS - 1)) for c in LETTER_COLOUR]
                    for k in range(LETTER_LEVELS)], dtype=np.int32)
    return {"x": x, "y": y, "w": crop.width, "h": crop.height, "data": idx, "palette": pal}


def write_c(images):
    names = ["DashSplashEmblem", "DashSplashLetterM", "DashSplashLetterR", "DashSplashLetter2"]
    out = ["/*",
           " * dash_splash.c - GENERATED by tools/splash/build_splash.py from",
           " * hw/dash_cluster/art. Do not edit; rerun the script.",
           " *",
           " * The power-on splash: the emblem, then one letter per node. Each image",
           " * carries its own palette, as RGB triples - the fade scales them every",
           " * frame, so they are kept as colours rather than as panel words.",
           " */",
           "",
           '#include "dash_splash.h"',
           ""]
    total = 0
    for name, im in zip(names, images):
        data = im["data"].tobytes()
        total += len(data) + im["palette"].size
        out.append("static const uint8_t %s_data[%d] = {" % (name, len(data)))
        for i in range(0, len(data), 32):
            out.append("    " + ", ".join("0x%02x" % b for b in data[i:i + 32]) + ",")
        out.append("};")
        out.append("")
        flat = [int(v) for v in im["palette"].flatten()]
        out.append("static const uint8_t %s_palette[%d] = {" % (name, len(flat)))
        for i in range(0, len(flat), 24):
            out.append("    " + ", ".join("%d" % v for v in flat[i:i + 24]) + ",")
        out.append("};")
        out.append("")
        out.append("const DashSplashImage_t %s = {" % name)
        out.append("    .X = %d, .Y = %d, .Width = %d, .Height = %d," % (im["x"], im["y"], im["w"], im["h"]))
        out.append("    .Data = %s_data," % name)
        out.append("    .Palette = %s_palette, .PaletteCount = %du," % (name, len(im["palette"])))
        out.append("};")
        out.append("")

    path = os.path.join(FIRMWARE, "src", "dash_splash.c")
    with open(path, "w", newline="\r\n") as f:
        f.write("\n".join(out))
    print("wrote %s: %d images, %.0f KB" % (os.path.relpath(path, FIRMWARE), len(images), total / 1024.0))
    for name, im in zip(names, images):
        print("  %-20s %3dx%3d at %3d,%3d  %3d colours" % (name, im["w"], im["h"], im["x"], im["y"], len(im["palette"])))


if __name__ == "__main__":
    main()
