#!/usr/bin/env python3
"""
Pre-render the gauge dials with LVGL on the PC and write src/dash_faces.c.

The dials are rendered at four times the panel's resolution, every sub-pixel is
snapped to the design colour it is nearest (which removes LVGL's own
anti-aliasing - it has no switch for that), and each panel pixel is then the
4x4 block under it: true area coverage, quantised to a few shades per pair of
colours so the whole dial fits a small palette. See reduce_face().

    python tools/face_render/build_faces.py      (from hw/dash_cluster/firmware)

Rerun after changing anything that decides how a dial looks: a gauge's tick
labels or placement in pages.c, UiGauge_CreateScale() in ui_gauge.c, or
lv_conf.h. The firmware refuses a face whose size no longer matches its
element and falls back to drawing the scale live, but it cannot notice changed
tick labels - a stale face would simply show the old numbers.

Builds LVGL itself with MSVC (found through vswhere, as the host tests do),
against the firmware's own lv_conf.h via tools/face_render/lv_conf.h. The
LVGL objects are cached in tools/face_render/build and rebuilt only when their
source or either lv_conf.h is newer; the application sources are always
rebuilt, since the cache cannot see header changes. Reruns take seconds.
"""

import os
import re
import subprocess
import sys

import numpy as np
from PIL import Image
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.normpath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(FIRMWARE, "src")
REPO = os.path.normpath(os.path.join(FIRMWARE, "..", "..", ".."))
LVGL = os.path.join(REPO, "external", "lvgl")
BUILD = os.path.join(HERE, "build")
OBJ = os.path.join(BUILD, "obj")
OUT = os.path.join(BUILD, "faces")
GENERATED = os.path.join(SRC, "dash_faces.c")

sys.path.insert(0, os.path.join(FIRMWARE, "test"))
from run_tests import find_vswhere_cl  # noqa: E402

# The firmware sources the renderer needs: the page tables and the dial.
# pages.c pulls in the signal store for its warning takeover.
APP_SOURCES = [
    os.path.join(HERE, "face_render.c"),
    os.path.join(HERE, "ui_gauge.c"),
    os.path.join(HERE, "dash_font_numeral_160.c"),
    os.path.join(HERE, "dash_font_legend_104.c"),
    os.path.join(SRC, "pages.c"),
    os.path.join(SRC, "signal_store.c"),
    os.path.join(SRC, "signals.c"),
]

CFLAGS = [
    "/nologo", "/O2", "/std:c11", "/w", "/utf-8",
    "/DLV_CONF_INCLUDE_SIMPLE",
    "/I" + HERE,            # its lv_conf.h, wrapping the firmware's
    "/I" + LVGL,            # "lvgl.h"
    "/I" + SRC,
]


def lvgl_sources():
    out = []
    for root, _, files in os.walk(os.path.join(LVGL, "src")):
        for f in files:
            if f.endswith(".c"):
                out.append(os.path.join(root, f))
    return sorted(out)


def obj_for(src):
    # Flatten the path: LVGL reuses file names across directories.
    rel = os.path.relpath(src, REPO).replace(os.sep, "_").replace(":", "")
    return os.path.join(OBJ, os.path.splitext(rel)[0] + ".obj")


# The object cache knows nothing about #include, so it is kept to where that
# is safe. The application sources - a handful of files sharing firmware
# headers like pages.h - are always rebuilt: a cached face_render.obj built
# against an older FaceElement_t once walked Pages[] with the wrong struct
# size and crashed. LVGL's objects are reused, but only while they are newer
# than both lv_conf.h files as well as their own source.
LV_CONFS = [os.path.join(FIRMWARE, "lv_conf.h"), os.path.join(HERE, "lv_conf.h")]


def compile_one(cl, env, src, cacheable):
    obj = obj_for(src)
    if cacheable and os.path.exists(obj):
        newest = max([os.path.getmtime(src)] + [os.path.getmtime(c) for c in LV_CONFS])
        if os.path.getmtime(obj) >= newest:
            return None
    r = subprocess.run([cl] + (CFLAGS if cacheable else APP_CFLAGS) + ["/c", src, "/Fo:" + obj],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        return "%s\n%s%s" % (src, r.stdout, r.stderr)
    return None


# OUR sources are compiled with warnings on, and with a pointer passed where it
# does not belong made an error. LVGL's stay at /w - they are someone else's,
# and thousands of warnings nobody here will fix would bury the ones that
# matter. This is not tidiness: with /w everywhere, a bool handed to LVGL as
# an object pointer compiled silently and the renderer crashed on its first
# page (C4047: differing levels of indirection).
APP_CFLAGS = [f for f in CFLAGS if f != "/w"] + ["/W3", "/we4047", "/we4024", "/we4013"]


# One table for every face, so any face can be dropped into the firmware's back
# buffer without remapping. 256 because the buffer holds a byte per pixel.
PALETTE_MAX = 256

# SHADES PER EDGE. Where two colours meet, a panel pixel is some mix of them;
# this is how many mixes the palette offers for each pair, the two pure colours
# included. 8 gives six in-between shades, which at 266 dpi is past what the eye
# resolves on a static edge. Each pair that actually meets costs LEVELS - 2
# entries.
RAMP_LEVELS = 8

# The colours the dial is DESIGNED in, read from ui_gauge.h so there is one
# definition. Black is the screen round the dial. The needle is not listed: it
# is drawn live and is not in any face.
DIAL_COLOURS = ["FACE", "MARK", "BAND", "RING"]
GAUGE_HEADER = os.path.join(SRC, "ui_gauge.h")

# A snapped render pixel should lie on the line between the two design colours
# it is nearest - it is an anti-aliased edge between them. One that does not is
# either a three-colour corner, which is expected and rare, or a colour nobody
# designed, which means something leaked into the dial (a theme default, say).
OFF_EDGE_TOLERANCE = 12.0
OFF_EDGE_MAX_FRACTION = 0.005


def design_colours():
    text = open(GAUGE_HEADER).read()
    found = dict(re.findall(r"#define\s+UI_GAUGE_(\w+)_COLOUR\s+\(0x([0-9A-Fa-f]{6})\)", text))
    names = ["BLACK"] + DIAL_COLOURS
    rgb = [(0, 0, 0)]
    for name in DIAL_COLOURS:
        if name not in found:
            sys.exit("%s: no UI_GAUGE_%s_COLOUR" % (GAUGE_HEADER, name))
        v = int(found[name], 16)
        rgb.append(((v >> 16) & 255, (v >> 8) & 255, v & 255))
    return names, np.array(rgb, dtype=np.float32)


def to565(rgb):
    r, g, b = (int(round(float(c))) for c in rgb)
    return (((r * 31 + 127) // 255) << 11) | (((g * 63 + 127) // 255) << 5) | ((b * 31 + 127) // 255)


# How far an anti-aliased pixel looks for the solid colours it sits between.
# LVGL's edges are a pixel or two wide, so a few render pixels is plenty.
SNAP_REACH = 2


def snap(render, bases, name):
    """Each render pixel to the index of the design colour it belongs to.

    WHY NOT SIMPLY THE NEAREST COLOUR. The design colours are nearly collinear -
    black, charcoal, ring grey and warm white are all greys - so a white tick's
    edge pixel at a third coverage IS ring grey, and nearest-colour snapping
    grows a grey fringe round every tick and numeral. So an edge pixel is
    snapped between the colours actually present, solid, around it: it is
    placed on the nearest line between two of those, and goes to whichever end
    it is closer to - which for a two-colour edge is a 50% coverage threshold,
    the same as rasterising without anti-aliasing at all."""
    hgt, wid = render.shape[:2]
    px = render.reshape(-1, 3).astype(np.float32)
    k = len(bases)

    exact = np.full(px.shape[0], -1, dtype=np.int64)
    for c in range(k):
        exact[(px == bases[c]).all(axis=1)] = c
    todo = np.nonzero(exact < 0)[0]

    # Which design colours appear solid within SNAP_REACH of each pixel.
    near = np.zeros((k, px.shape[0]), dtype=bool)
    r = SNAP_REACH
    for c in range(k):
        m = np.pad((exact == c).reshape(hgt, wid), r)
        win = np.lib.stride_tricks.sliding_window_view(m, (2 * r + 1, 2 * r + 1))
        near[c] = win.any(axis=(2, 3)).reshape(-1)

    q = px[todo]
    best = np.full(len(todo), np.inf, dtype=np.float32)
    pick = np.zeros(len(todo), dtype=np.int64)
    for a in range(k):
        for b in range(a + 1, k):
            ab = bases[b] - bases[a]
            t = np.clip(((q - bases[a]) * ab).sum(axis=1) / float((ab * ab).sum()), 0, 1)
            d = np.abs(bases[a] + ab * t[:, None] - q).max(axis=1)
            # Only pairs whose colours are both around; if a pixel has fewer
            # than two solid neighbours, any pair will do.
            ok = (near[a, todo] & near[b, todo]) | (near[:, todo].sum(axis=0) < 2)
            d = np.where(ok, d, np.inf)
            better = d < best
            best[better] = d[better]
            pick[better] = np.where(t[better] < 0.5, a, b)

    out = exact.copy()
    out[todo] = pick
    off = float((best > OFF_EDGE_TOLERANCE).sum()) / px.shape[0]
    print("%s: %.2f%% of render pixels were anti-aliased and snapped; %.3f%% not on any edge"
          % (name, 100.0 * len(todo) / px.shape[0], 100.0 * off))
    if off > OFF_EDGE_MAX_FRACTION:
        sys.exit("%s: %.2f%% of pixels are not a mix of two design colours - "
                 "something undesigned is being drawn" % (name, 100.0 * off))
    return out.reshape(hgt, wid)


def reduce_face(render, w, h, scale, bases, name):
    """A (h*scale, w*scale, 3) render to one palette key per panel pixel.

    Keys are ("solid", i) or ("ramp", i, j, level) with i < j and level in
    1..RAMP_LEVELS-2, counting from colour i towards colour j. Also returns a
    plain box filter of the render, for comparison."""
    idx = snap(render, bases, name)
    k = len(bases)
    blocks = idx.reshape(h, scale, w, scale)
    counts = np.stack([(blocks == c).sum(axis=(1, 3)) for c in range(k)], axis=-1)
    area = scale * scale

    # Few distinct coverage patterns occur, so decide each once.
    patterns, inverse = np.unique(counts.reshape(-1, k), axis=0, return_inverse=True)
    keys = []
    for pat in patterns:
        present = [int(c) for c in np.argsort(-pat, kind="stable") if pat[c] > 0]
        if len(present) == 1:
            keys.append(("solid", present[0]))
            continue
        a, b = present[0], present[1]
        n = {a: int(pat[a]), b: int(pat[b])}
        # A third colour in one panel pixel - a tick's tip at a band's edge -
        # goes to whichever of the two main colours it looks more like.
        for c in present[2:]:
            da = float(np.abs(bases[c] - bases[a]).sum())
            db = float(np.abs(bases[c] - bases[b]).sum())
            n[a if da <= db else b] += int(pat[c])
        lo, hi = min(a, b), max(a, b)
        level = int(n[hi] * (RAMP_LEVELS - 1) / area + 0.5)
        if level == 0:
            keys.append(("solid", lo))
        elif level == RAMP_LEVELS - 1:
            keys.append(("solid", hi))
        else:
            keys.append(("ramp", lo, hi, level))

    face = [keys[m] for m in inverse.reshape(-1)]
    boxed = render.reshape(h, scale, w, scale, 3).astype(np.float32).mean(axis=(1, 3))
    return face, boxed


def key_rgb(key, bases):
    if key[0] == "solid":
        return bases[key[1]]
    _, i, j, level = key
    return bases[i] + (bases[j] - bases[i]) * (level / (RAMP_LEVELS - 1))


def emit_bytes(lines, data, per_line):
    for k in range(0, len(data), per_line):
        lines.append("    " + ",".join("0x%02x" % b for b in data[k:k + per_line]) + ",")


def write_generated(manifest):
    names, bases = design_colours()
    scale = manifest[0][5]
    faces = []
    for page, view, elem, w, h, sc in manifest:
        name = "face_p%d_v%d_e%d" % (page, view, elem)
        raw = open(os.path.join(OUT, "p%d_v%d_e%d.rgb" % (page, view, elem)), "rb").read()
        if len(raw) != w * h * sc * sc * 3:
            sys.exit("%s: %d bytes, expected %d" % (name, len(raw), w * h * sc * sc * 3))
        render = np.frombuffer(raw, dtype=np.uint8).reshape(h * sc, w * sc, 3)
        keys, boxed = reduce_face(render, w, h, sc, bases, name)
        faces.append((page, view, elem, w, h, name, keys, boxed))

    # The palette: black first, so a cleared buffer is black; then the design
    # colours the faces use; then the shades, in a stable order. Keyed by RGB565
    # value, so two shades that land on the same 565 colour share an entry.
    used = set()
    for f in faces:
        used.update(f[6])
    ordered = [("solid", c) for c in range(len(bases))]
    ordered += sorted(k for k in used if k[0] == "ramp")
    palette, lookup = [], {}
    for key in ordered:
        if key[0] == "solid" and key not in used and key != ("solid", 0):
            continue
        v = to565(key_rgb(key, bases))
        if v not in lookup:
            lookup[v] = len(palette)
            palette.append(v)
    if len(palette) > PALETTE_MAX:
        sys.exit("faces need %d colours - the firmware's paletted buffer holds %d"
                 % (len(palette), PALETTE_MAX))

    pairs = {}
    for k in used:
        if k[0] == "ramp":
            pairs.setdefault((names[k[1]], names[k[2]]), set()).add(k[3])
    for (a, b), levels in sorted(pairs.items()):
        print("  %-5s <-> %-5s  %d shades between" % (a, b, len(levels)))

    # Each entry is the panel's two bytes read little-endian, so storing it on
    # the little-endian RP2350 puts them back in wire order, high byte first.
    wire = [((v & 0xFF) << 8) | (v >> 8) for v in palette]
    wire += [0] * (PALETTE_MAX - len(wire))

    lines = [
        "/*******************************************************************************",
        " * GENERATED by tools/face_render/build_faces.py - do not edit, regenerate.",
        " *",
        " * The gauge dials, rendered on the PC by LVGL at %dx the panel's resolution" % scale,
        " * (LVGL is not in the firmware) with every sub-pixel snapped to the nearest",
        " * design colour, then reduced: each panel pixel is the %dx%d block beneath it," % (scale, scale),
        " * as one of %d shades between the two colours that meet there." % RAMP_LEVELS,
        " *",
        " * One byte per pixel, indexing a single table shared by every face.",
        " * %d of %d palette entries used. Index 0 is black." % (len(palette), PALETTE_MAX),
        " ******************************************************************************/",
        "",
        "#include \"dash_faces.h\"",
        "",
        "/* Each entry is the panel's two bytes read little-endian - (second << 8) |",
        "   first - so that storing it on the little-endian RP2350 puts the bytes back",
        "   in wire order, high byte first. Unused entries are black. */",
        "const uint16_t DashFacePalette[DASH_FACE_PALETTE_SIZE] =",
        "{",
    ]
    for k in range(0, PALETTE_MAX, 8):
        lines.append("    " + ", ".join("0x%04x" % v for v in wire[k:k + 8]) + ",")
    lines += ["};", "", "const uint16_t DashFacePaletteUsed = %du;" % len(palette), ""]

    entries = []
    for page, view, elem, w, h, name, keys, boxed in faces:
        colour = {k: key_rgb(k, bases) for k in set(keys)}
        want = [to565(colour[k]) for k in keys]
        indices = bytes(lookup[v] for v in want)

        # Byte order, checked the way the firmware will read it.
        decoded = [int.from_bytes(wire[n].to_bytes(2, "little"), "big") for n in indices]
        if decoded != want:
            sys.exit("%s: palette does not decode to the intended colours" % name)

        # How far the reduction strays from a plain box filter of LVGL's own
        # anti-aliased render - the price of the small palette, in 8-bit units.
        got = np.array([colour[k] for k in keys], dtype=np.float32).reshape(h, w, 3)
        err = np.abs(got - boxed).max(axis=2)
        print("%s: %dx%d, %d colours; differs from a box filter by mean %.2f, "
              "99th pct %.0f, max %.0f"
              % (name, w, h, len(set(indices)), float(err.mean()),
                 float(np.percentile(err, 99)), float(err.max())))

        Image.fromarray(np.clip(got + 0.5, 0, 255).astype(np.uint8)).save(
            os.path.join(OUT, "p%d_v%d_e%d_preview.png" % (page, view, elem)))

        lines.append("static const uint8_t %s_map[] = {" % name)
        emit_bytes(lines, indices, 32)
        lines += ["};", "",
                  "static const DashImage_t %s = {" % name,
                  "    .Width = %d," % w,
                  "    .Height = %d," % h,
                  "    .Stride = %d," % w,
                  "    .Data = %s_map," % name,
                  "};", ""]
        entries.append("    { %d, %d, %d, %d, %d, &%s }," % (page, view, elem, w, h, name))

    lines += ["const DashFace_t DashFaces[] =", "{"] + entries + ["};", "",
              "const uint8_t DashFaceCount = (uint8_t)(sizeof(DashFaces) / sizeof(DashFaces[0]));", ""]

    with open(GENERATED, "w", newline="\r\n") as f:
        f.write("\n".join(lines))

    print("palette: %d of %d entries" % (len(palette), PALETTE_MAX))


def main():
    if not os.path.exists(os.path.join(LVGL, "lvgl.h")):
        sys.exit("LVGL not found at %s" % LVGL)

    cl, env = find_vswhere_cl()
    if cl is None:
        sys.exit("MSVC not found - install the Visual Studio Build Tools")

    os.makedirs(OBJ, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)

    sources = lvgl_sources() + APP_SOURCES
    print("compiling %d sources with MSVC (cached objects are reused)..." % len(sources))
    workers = max(2, (os.cpu_count() or 4))
    with ThreadPoolExecutor(max_workers=workers) as pool:
        app = set(APP_SOURCES)
        errors = [e for e in pool.map(lambda s: compile_one(cl, env, s, s not in app), sources) if e]
    if errors:
        for e in errors[:5]:
            print(e)
        sys.exit("%d source(s) failed to compile" % len(errors))

    exe = os.path.join(BUILD, "face_render.exe")
    rsp = os.path.join(BUILD, "link.rsp")
    with open(rsp, "w") as f:
        f.write("\n".join('"%s"' % obj_for(s) for s in sources))
    r = subprocess.run([cl, "/nologo", "/Fe:" + exe, "@" + rsp],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-4000:])
        sys.exit("link failed")

    r = subprocess.run([exe, OUT], env=env, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.returncode != 0:
        sys.stderr.write(r.stderr)
        sys.exit("renderer failed")

    manifest = []
    for line in open(os.path.join(OUT, "faces.txt")):
        if line.strip():
            manifest.append(tuple(int(x) for x in line.split()))

    write_generated(manifest)
    print("wrote %s (%d face(s))" % (os.path.relpath(GENERATED, FIRMWARE), len(manifest)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
