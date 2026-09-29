#!/usr/bin/env python3
"""
Generate carrier.kicad_pcb - the dash node's piggyback board - from the design
below, against the nets the schematic actually has.

    python gen_pcb.py            write the board
    python gen_pcb.py --check    ...then run KiCad's DRC and check every pad's
                                 net against the schematic's netlist

WHY A GENERATOR. Same reason as gen_schematic.py: the design is a table beside
the reasoning in schematic.txt, UUIDs come from what they identify so a
regenerate is a no-op in git, and the checks are mechanical. Open the result
in Pcbnew and push things around freely - nothing here needs running again
unless the design changes.

WHERE THE NETS COME FROM. Not from a table here: `kicad-cli sch export
netlist` is run on carrier.kicad_sch and every pad's net is taken from that.
So the board cannot disagree with the schematic about what connects to what -
the worst it can do is fail to route something, which DRC then reports as an
unconnected item.

THE STACK is 4 layers to JLCPCB's standard 4-layer rules (0.15 mm track and
gap, 0.3 mm drill, 0.6 mm via):

    F.Cu    parts and signal routing, plus a GND pour in the gaps
    In1.Cu  GND plane
    In2.Cu  GND plane
    B.Cu    signal routing where the top runs out, plus a GND pour

Two ground planes rather than a plane and a power layer: every supply here is
a short run that a wide top-layer track carries perfectly well, while the
things that actually care - the switcher's return path and the CAN pair -
both want an unbroken reference directly beneath them. A split power plane
would have bought nothing and put a gap under one of them.
"""

import argparse
import copy
import math
import os
import re
import subprocess
import sys
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_schematic import (KICAD_CLI, dump, find_all, parse, unquote)  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "carrier.kicad_pcb")
SCH = os.path.join(HERE, "carrier.kicad_sch")
FOOTPRINTS = r"C:\Program Files\KiCad\9.0\share\kicad\footprints"

# The board: 55 x 35 mm, its top left corner here. Provisional until the
# module is measured - the outline and the socket's position are the two
# things that move then, and neither disturbs the blocks below.
#
# WHY NOT THE 45 x 30 THIS STARTED AT. Three parts set the floor and none of
# them is negotiable at that size: each right-angle Micro-Fit is 13.7 mm deep,
# so the two of them own the left edge; the module's socket is 21.4 mm of the
# right edge; and U1 (12.2 mm) and C4 (12.3 mm) are wide enough that they
# cannot share the remaining middle and still leave lanes to route in. 55 x 35
# is still nothing in a 180 x 50 DIN aperture. The alternative - an SMD
# regulator and a smaller hold-up cap - would fit 45 x 30, at the cost of the
# thing C4 is there for.
X0, Y0 = 100.0, 60.0
W, H = 55.0, 35.0
CORNER = 1.5            # outline corner radius, drawn as a chamfer
HOLE = 2.2              # M2 mounting holes
HOLES = [(2.5, 2.5), (52.5, 2.5), (2.5, 32.5), (52.5, 32.5)]

TRACK = 0.25            # ordinary signal track
POWER_TRACK = 0.8       # 12 V in, VIN, +5 V, VBUS - a 1 A supply
VIA = (0.6, 0.3)        # pad, drill


# ---------------------------------------------------------------------------
# WHERE EVERY PART SITS:  ref: (x, y, rotation, layer)
#
# Coordinates are the board's own, in millimetres from its top left corner,
# so a part keeps its place if the outline moves. Read left to right: the loom
# arrives on the left edge, the supply runs across the top, CAN sits in the
# middle, the EEPROM bottom right beside the socket the module plugs into.
# ---------------------------------------------------------------------------

PLACE = {
    # The loom, in and out: right-angle Micro-Fit turned so the cables leave
    # over the LEFT EDGE. Not up: the module sits directly in front of this
    # board, and a vertical housing would point its cable straight at it.
    "J2": (9.7, 12.1, 90, "F.Cu"),
    "J3": (9.7, 25.5, 90, "F.Cu"),

    # 12 V in, along the top: fuse, the reverse-polarity FET with its gate
    # parts, then the clamp
    "F1": (7.6, 2.6, 0, "F.Cu"),
    "Q1": (12.6, 2.8, 0, "F.Cu"),
    "D5": (17.1, 3.0, 90, "F.Cu"),
    "R2": (19.6, 3.0, 90, "F.Cu"),
    "D1": (24.6, 3.2, 0, "F.Cu"),

    # 5 V. C1 is the switcher's input cap, so it sits under U1's own Vin pin
    # rather than anywhere more convenient - that loop is the one that matters.
    "C1": (30.6, 3.2, 90, "F.Cu"),
    "U1": (36.6, 6.6, 0, "F.Cu"),
    "C2": (47.6, 3.0, 0, "F.Cu"),
    "C3": (47.6, 6.0, 0, "F.Cu"),
    "D2": (44.0, 13.0, 90, "F.Cu"),
    "C4": (39.0, 29.5, 0, "F.Cu"),

    # CAN in the middle. The pair leaves U2 towards the loom, so the
    # termination pads sit past D3, at the very end of the pair.
    "C5": (20.0, 8.0, 0, "F.Cu"),
    "U2": (20.0, 12.0, 0, "F.Cu"),
    "C6": (22.3, 17.2, 0, "F.Cu"),
    "R1": (26.5, 10.5, 0, "F.Cu"),
    "D3": (26.5, 14.0, 90, "F.Cu"),
    "R3": (26.5, 19.0, 0, "F.Cu"),
    "R4": (26.5, 22.0, 180, "F.Cu"),
    "C7": (30.0, 20.5, 90, "F.Cu"),

    # the carrier's own memory, along the bottom
    "U3": (20.0, 28.0, 0, "F.Cu"),
    "C9": (25.5, 28.0, 0, "F.Cu"),
    "R5": (29.5, 26.5, 0, "F.Cu"),
    "R6": (29.5, 31.0, 0, "F.Cu"),

    # The module. 21.4 mm of socket, so it runs down the right hand edge -
    # pin 1 at the top, which puts VBUS (pin 8) at the bottom beside C4 and
    # the I2C pair at the top. Both move when the module is measured.
    "J1": (51.5, 7.5, 0, "F.Cu"),
}


# ---------------------------------------------------------------------------
# Reading the schematic's netlist
# ---------------------------------------------------------------------------

def netlist():
    """{(ref, pad): net} and {ref: footprint}, from the schematic itself."""
    build = os.path.join(HERE, "build")
    os.makedirs(build, exist_ok=True)
    path = os.path.join(build, "carrier.net")
    subprocess.run([KICAD_CLI, "sch", "export", "netlist", "--format",
                    "kicadsexpr", "--output", path, SCH],
                   capture_output=True, text=True, check=True)
    text = open(path, encoding="utf-8").read()

    comps = re.findall(
        r'\(comp \(ref "([^"]+)"\)\s*\(value "([^"]*)"\)\s*'
        r'\(footprint "([^"]+)"\)', text)
    footprints = dict((ref, fp) for ref, _v, fp in comps)
    values = dict((ref, v) for ref, v, _fp in comps)

    nets, net = {}, None
    for line in text.splitlines():
        named = re.search(r'\(net \(code "\d+"\) \(name "([^"]+)"\)', line)
        if named:
            # Keep the name exactly as the schematic gives it, leading "/" and
            # all: a local net is "/VIN" there, and a board that calls it "VIN"
            # fails DRC's parity check on every pad of it.
            net = named.group(1)
        node = re.search(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', line)
        if node and net:
            nets[node.groups()] = net
    return nets, footprints, values


# ---------------------------------------------------------------------------
# Footprints
# ---------------------------------------------------------------------------

FPS = {}


def load_footprint(lib_id):
    if lib_id in FPS:
        return FPS[lib_id]
    lib, name = lib_id.split(":")
    path = os.path.join(FOOTPRINTS, lib + ".pretty", name + ".kicad_mod")
    if not os.path.exists(path):
        sys.exit("no such footprint: %s" % lib_id)
    FPS[lib_id] = parse(open(path, encoding="utf-8").read())[0][0]
    return FPS[lib_id]


def place(px, py, rot):
    """A footprint-local point to board coordinates. KiCad turns a footprint
    counter-clockwise on screen, where y runs down, which is this."""
    a = math.radians(rot)
    return (px * math.cos(a) + py * math.sin(a),
            -px * math.sin(a) + py * math.cos(a))


PADPOS = {}


PADBOX = {}


def pad_positions():
    for ref, (x, y, rot, _layer) in PLACE.items():
        for pad in find_all(load_footprint(FOOTPRINT[ref]), "pad"):
            number = unquote(pad[1])
            at = next(find_all(pad, "at"))
            size = next(find_all(pad, "size"))
            dx, dy = place(float(at[1]), float(at[2]), rot)
            key = "%s.%s" % (ref, number)
            PADPOS[key] = (round(X0 + x + dx, 3), round(Y0 + y + dy, 3))
            w, h = float(size[1]), float(size[2])
            if rot in (90, 270):
                w, h = h, w
            PADBOX[key] = (w, h, unquote(pad[2]) != "smd")
    return PADPOS


# ---------------------------------------------------------------------------
# Routing
#
# A run is a net, a width, a starting layer, then a path: pad names, points in
# board millimetres, and "via" wherever it changes layer (F.Cu <-> B.Cu).
# Nothing is routed for GND - the pours on all four layers do that, tied
# together by the through-hole pads and the stitching vias below.
#
# The scheme, where a straight shot is not available: horizontals go on B.Cu
# and verticals on F.Cu. B.Cu carries nothing but the pour and the
# through-hole pads, so a long run across the board is free there, and two
# nets crossing at right angles never meet.
# ---------------------------------------------------------------------------

STITCH = [(5.0, 18.5), (14.0, 32.0), (33.0, 12.5), (33.0, 21.0),
          (38.0, 21.0), (49.0, 4.0), (51.0, 31.0), (7.0, 31.0)]

# WHAT IS ROUTED HERE, AND WHAT IS NOT. The supply chain and the short local
# nets are below and verified. The CAN pair, the two I2C lines and the two
# serial lines to the module are deliberately NOT routed: CANH/CANL want
# hand-routing as a matched pair with equal lengths and no stubs, and the four
# module signals all funnel into one edge where a mouse beats a table of
# coordinates. Pcbnew's ratsnest shows exactly what is left, and every pad
# already carries the right net, so the interactive router has everything it
# needs. `--check` lists the unrouted nets rather than hiding them.

TRACKS = [
    # 12 V from the loom, down the left edge and out to the second connector
    # x = 7.65 threads the 1.5 mm gap between the connector's 3 mm mounting
    # peg at x = 5.38 and F1's own pad at 8.4 - the pre-check found this.
    ("+12V", POWER_TRACK, "F.Cu", ["F1.1", (7.65, 2.6), (7.65, 12.1), "J2.1"]),
    ("+12V", POWER_TRACK, "F.Cu", [(7.65, 12.1), (7.65, 25.5), "J3.1"]),

    # through the fuse to the FET's source, and on to the Zener's cathode
    ("/12V_F", 0.5, "F.Cu", ["F1.2", (9.0, 3.75), "Q1.2",
                             (11.66, 4.8), (17.1, 4.8), "D5.1"]),

    # the gate: pulled down through R2, clamped by D5
    ("/GATE", 0.3, "F.Cu", ["Q1.1", (11.66, 1.35), "D5.2",
                            (19.6, 1.35), "R2.2"]),

    # VIN. D5 and R2 sit across the direct path, so it drops to the back for
    # the run east and comes up at each part it feeds.
    ("/VIN", POWER_TRACK, "F.Cu", ["Q1.3", (15.6, 2.8), "via",
                                   (15.6, 6.4), (36.6, 6.4), "U1.1"]),
    ("/VIN", 0.5, "B.Cu", [(26.75, 6.4), "via", (26.75, 4.6), "D1.2"]),
    ("/VIN", 0.5, "B.Cu", [(30.6, 6.4), "via", (30.6, 5.3), "C1.1"]),

    # +5 V: out of U1, along to D2, with the two decouplers tapped off it
    ("+5V", POWER_TRACK, "F.Cu", ["U1.3", (44.0, 6.6), "D2.2"]),
    ("+5V", 0.4, "F.Cu", [(44.0, 6.6), (44.0, 6.0), "C3.1"]),
    ("+5V", 0.4, "F.Cu", [(45.5, 6.0), (45.5, 3.0), "C2.1"]),
    # and across the back to the transceiver, which is the far side of the board
    ("+5V", 0.5, "B.Cu", ["U1.3", (41.68, 9.6), (19.1, 9.6), "via",
                          (19.1, 8.3)]),
    ("+5V", 0.4, "F.Cu", [(19.1, 9.6), (19.1, 12.64), "U2.3"]),

    # VBUS: behind the diode, to the hold-up cap and out to the module
    ("VBUS", POWER_TRACK, "F.Cu", ["D2.1", (44.0, 17.0), (34.0, 17.0),
                                   (34.0, 28.3)]),
    ("VBUS", POWER_TRACK, "F.Cu", [(44.0, 17.0), (48.6, 17.0),
                                   (48.6, 25.28), "J1.8"]),

    # the termination midpoint, and its capacitor to ground
    ("/TERM_M", 0.3, "F.Cu", ["R3.2", "R4.1"]),
    ("/TERM_M", 0.3, "F.Cu", [(27.41, 21.28), "C7.1"]),

    # TXD through its series resistor - under the transceiver on the back
    ("/TXD_R", 0.3, "F.Cu", ["U2.1", (17.53, 8.6), "via",
                             (27.33, 8.6), "via", (27.33, 9.6), "R1.2"]),
]


def emit_tracks(net_of):
    """Path items to track segments and vias, checked as they are built."""
    out, segments, vias = [], [], []

    for net, width, layer, path in TRACKS:
        here = None
        for item in path:
            if item == "via":
                vias.append((here, net))
                layer = "B.Cu" if layer == "F.Cu" else "F.Cu"
                continue
            point = PADPOS[item] if isinstance(item, str) else \
                (round(X0 + item[0], 3), round(Y0 + item[1], 3))
            if here is not None and here != point:
                segments.append((here, point, layer, width, net))
            here = point

    for a, b, layer, width, net in segments:
        out.append("""\t(segment
\t\t(start %.3f %.3f)
\t\t(end %.3f %.3f)
\t\t(width %.2f)
\t\t(layer "%s")
\t\t(net %d)
\t\t(uuid "%s")
\t)""" % (a[0], a[1], b[0], b[1], width, layer, NETNUM[net],
         uid("seg:%s:%.2f:%.2f:%.2f:%.2f:%s" % ((net,) + a + b + (layer,)))))

    for at, net in vias + [((round(X0 + x, 3), round(Y0 + y, 3)), "GND")
                           for x, y in STITCH]:
        out.append("""\t(via
\t\t(at %.3f %.3f)
\t\t(size %.2f)
\t\t(drill %.2f)
\t\t(layers "F.Cu" "B.Cu")
\t\t(net %d)
\t\t(uuid "%s")
\t)""" % (at[0], at[1], VIA[0], VIA[1], NETNUM[net],
         uid("via:%s:%.2f:%.2f" % ((net,) + at))))

    return out, segments, vias


# ---------------------------------------------------------------------------
# The geometric pre-check
#
# DRC is the authority, but it needs the whole board written and takes a
# minute. This says the same thing about clearance in a second, and names the
# two things involved, which is what makes a routing mistake quick to fix.
# ---------------------------------------------------------------------------

CLEARANCE = 0.2


def seg_point_distance(a, b, p):
    ax, ay, bx, by = a[0], a[1], b[0], b[1]
    dx, dy = bx - ax, by - ay
    length = dx * dx + dy * dy
    t = 0.0 if length == 0 else max(0.0, min(1.0, ((p[0] - ax) * dx +
                                                   (p[1] - ay) * dy) / length))
    return math.hypot(p[0] - (ax + t * dx), p[1] - (ay + t * dy))


def seg_box_distance(a, b, centre, w, h):
    """Distance from a segment to an axis-aligned box, 0 if they meet."""
    x0, x1 = centre[0] - w / 2, centre[0] + w / 2
    y0, y1 = centre[1] - h / 2, centre[1] + h / 2
    best = float("inf")
    corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    for i in range(4):
        c, d = corners[i], corners[(i + 1) % 4]
        # segment against each edge: if they cross, distance is zero
        if segments_cross(a, b, c, d):
            return 0.0
        best = min(best, seg_point_distance(a, b, c),
                   seg_point_distance(c, d, a), seg_point_distance(c, d, b))
    if x0 <= a[0] <= x1 and y0 <= a[1] <= y1:
        return 0.0
    return best


def side(a, b, p):
    return ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]))


def segments_cross(a, b, c, d):
    d1, d2 = side(c, d, a), side(c, d, b)
    d3, d4 = side(a, b, c), side(a, b, d)
    return ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0))


def precheck(segments, vias):
    """Every track and via against every pad and every other net's copper."""
    problems = []

    pads = [(key, PADPOS[key], PADBOX[key], NETS.get(tuple(key.split("."))))
            for key in PADPOS if key in PADBOX]

    for a, b, layer, width, net in segments:
        for key, at, (w, h, through), pad_net in pads:
            if pad_net == net or (not through and layer != "F.Cu"):
                continue
            gap = seg_box_distance(a, b, at, w, h) - width / 2
            if gap < CLEARANCE:
                problems.append("%-8s track at (%.1f,%.1f)-(%.1f,%.1f) %s is "
                                "%.2f from %s [%s]"
                                % (net, a[0] - X0, a[1] - Y0, b[0] - X0,
                                   b[1] - Y0, layer, gap, key,
                                   pad_net or "no net"))

    for i, (a, b, layer, width, net) in enumerate(segments):
        for c, d, other_layer, other_width, other_net in segments[i + 1:]:
            if other_net == net or other_layer != layer:
                continue
            gap = min(seg_point_distance(a, b, c), seg_point_distance(a, b, d),
                      seg_point_distance(c, d, a), seg_point_distance(c, d, b))
            if segments_cross(a, b, c, d):
                gap = 0.0
            gap -= (width + other_width) / 2
            if gap < CLEARANCE:
                problems.append("%-8s track (%.1f,%.1f)-(%.1f,%.1f) %s is "
                                "%.2f from %s track (%.1f,%.1f)-(%.1f,%.1f)"
                                % (net, a[0] - X0, a[1] - Y0, b[0] - X0,
                                   b[1] - Y0, layer, gap, other_net,
                                   c[0] - X0, c[1] - Y0, d[0] - X0, d[1] - Y0))

    everything = vias + [((round(X0 + x, 3), round(Y0 + y, 3)), "GND")
                         for x, y in STITCH]
    for at, net in everything:
        for key, pad_at, (w, h, _t), pad_net in pads:
            if pad_net == net:
                continue
            gap = (seg_box_distance(at, at, pad_at, w, h) - VIA[0] / 2)
            if gap < CLEARANCE:
                problems.append("%-8s via at (%.1f,%.1f) is %.2f from %s [%s]"
                                % (net, at[0] - X0, at[1] - Y0, gap, key,
                                   pad_net or "no net"))
        for a, b, layer, width, seg_net in segments:
            if seg_net == net:
                continue
            gap = seg_point_distance(a, b, at) - width / 2 - VIA[0] / 2
            if gap < CLEARANCE:
                problems.append("%-8s via at (%.1f,%.1f) is %.2f from %s "
                                "track (%.1f,%.1f)-(%.1f,%.1f) %s"
                                % (net, at[0] - X0, at[1] - Y0, gap, seg_net,
                                   a[0] - X0, a[1] - Y0, b[0] - X0, b[1] - Y0,
                                   layer))

    return problems


# ---------------------------------------------------------------------------
# Writing it out
# ---------------------------------------------------------------------------

NAMESPACE = uuid.UUID("6f1d4b3a-0001-4000-8000-746f796f7475")


def uid(key):
    return str(uuid.uuid5(NAMESPACE, key))


LAYERS = [
    (0, "F.Cu", "signal"), (1, "In1.Cu", "signal"), (2, "In2.Cu", "signal"),
    (31, "B.Cu", "signal"),
    (32, "B.Adhes", "user", "B.Adhesive"), (33, "F.Adhes", "user", "F.Adhesive"),
    (34, "B.Paste", "user"), (35, "F.Paste", "user"),
    (36, "B.SilkS", "user", "B.Silkscreen"), (37, "F.SilkS", "user", "F.Silkscreen"),
    (38, "B.Mask", "user"), (39, "F.Mask", "user"),
    (40, "Dwgs.User", "user", "User.Drawings"),
    (41, "Cmts.User", "user", "User.Comments"),
    (42, "Eco1.User", "user", "User.Eco1"), (43, "Eco2.User", "user", "User.Eco2"),
    (44, "Edge.Cuts", "user"), (45, "Margin", "user"),
    (46, "B.CrtYd", "user", "B.Courtyard"), (47, "F.CrtYd", "user", "F.Courtyard"),
    (48, "B.Fab", "user"), (49, "F.Fab", "user"),
]

FOOTPRINT = {}
VALUE = {}
PROBLEMS = []
ROUTED = set()
NETS = {}
NETNUM = {}


def layer_block():
    out = []
    for row in LAYERS:
        name = '"%s"' % row[3] if len(row) > 3 else ""
        out.append("\t\t(%d %s %s%s)" % (row[0], row[1], row[2],
                                         " " + name if name else ""))
    return "\n".join(out)


def outline():
    """A chamfered rectangle - eight segments, no arcs, so a fab that dislikes
    arcs in an outline has nothing to complain about."""
    c = CORNER
    pts = [(c, 0), (W - c, 0), (W, c), (W, H - c), (W - c, H), (c, H),
           (0, H - c), (0, c)]
    out = []
    for i in range(len(pts)):
        a, b = pts[i], pts[(i + 1) % len(pts)]
        out.append("""\t(gr_line
\t\t(start %.3f %.3f)
\t\t(end %.3f %.3f)
\t\t(stroke (width 0.1) (type default))
\t\t(layer "Edge.Cuts")
\t\t(uuid "%s")
\t)""" % (X0 + a[0], Y0 + a[1], X0 + b[0], Y0 + b[1],
         uid("edge:%d" % i)))
    return out


def hole_places():
    """H1..H4, as PLACE entries - so the holes go through the same path as
    every other part and DRC's parity check finds them in the schematic."""
    return dict(("H%d" % (i + 1), (x, y, 0, "F.Cu"))
                for i, (x, y) in enumerate(HOLES))


def footprint_block(ref):
    """The library footprint, kept whole. Only four things are changed: where
    it sits, the Reference and Value text, the uuids, and the net on each pad.
    Everything else - courtyard, silkscreen, fab outline, 3D model, the smd or
    through_hole attribute - comes across untouched, because DRC compares the
    board's copy against the library and reports any difference."""
    lib_id = FOOTPRINT[ref]
    x, y, rot, layer = PLACE[ref]
    fp = load_footprint(lib_id)
    fields = {"Reference": ref, "Value": VALUE.get(ref, ref)}

    body = []
    for child in fp[1:]:
        if not isinstance(child, list):
            continue
        what = child[0]
        if what in ("version", "generator", "generator_version",
                    "embedded_fonts"):
            continue
        if what == "property" and unquote(child[1]) in fields:
            name = unquote(child[1])
            child = [child[0], child[1], '"' + fields[name]] + [
                c for c in child[3:]
                if not (isinstance(c, list) and c[0] == "uuid")]
            child = child + [["uuid", '"' + uid("prop:%s.%s" % (ref, name))]]
        elif what == "pad":
            number = unquote(child[1])
            net = NETS.get((ref, number))
            # Deep copy: the library tree is cached and shared between every
            # part that uses this footprint.
            child = [copy.deepcopy(c) for c in child
                     if not (isinstance(c, list) and c[0] == "uuid")]
            # A pad's angle is measured from the BOARD, not from the footprint
            # it belongs to, so a rotated part's pads carry its rotation on
            # top of their own. Leave it off and the geometry still comes out
            # right, but KiCad's comparison against the library reports every
            # rotated footprint as modified.
            if rot:
                at = next(find_all(child, "at"))
                angle = float(at[3]) if len(at) > 3 else 0.0
                at[3:] = ["%g" % ((angle + rot) % 360)]
            if net:
                child = child + [["net", str(NETNUM[net]), '"' + net]]
            child = child + [["uuid", '"' + uid("pad:%s.%s" % (ref, number))]]
        body.append("\t\t" + dump(child, 3))

    return """\t(footprint "%s"
\t\t(layer "%s")
\t\t(uuid "%s")
\t\t(at %.3f %.3f %d)
%s
\t)""" % (lib_id, layer, uid("fp:" + ref), X0 + x, Y0 + y, rot,
         "\n".join(body))


def zone(layer_names, name, net, priority=0):
    """A pour. The two inner layers are solid ground; the outer two fill the
    gaps between tracks, which is what lets an SMD ground pad reach the plane
    without a via of its own."""
    pts = " ".join("(xy %.3f %.3f)" % (X0 + x, Y0 + y) for x, y in
                   [(0.3, 0.3), (W - 0.3, 0.3), (W - 0.3, H - 0.3), (0.3, H - 0.3)])
    return """\t(zone
\t\t(net %d)
\t\t(net_name "%s")
\t\t(layers %s)
\t\t(uuid "%s")
\t\t(name "%s")
\t\t(hatch edge 0.5)
\t\t(priority %d)
\t\t(connect_pads (clearance 0.25))
\t\t(min_thickness 0.2)
\t\t(filled_areas_thickness no)
\t\t(fill yes
\t\t\t(thermal_gap 0.3)
\t\t\t(thermal_bridge_width 0.5)
\t\t)
\t\t(polygon (pts %s))
\t)""" % (NETNUM[net], net, " ".join('"%s"' % n for n in layer_names),
         uid("zone:" + name), name, priority, pts)


def build():
    nets, footprints, values = netlist()
    NETS.update(nets)
    FOOTPRINT.update(footprints)
    VALUE.update(values)
    PLACE.update(hole_places())
    for ref in PLACE:
        if ref not in FOOTPRINT:
            sys.exit("%s is placed but not in the schematic" % ref)
    for ref in FOOTPRINT:
        if ref not in PLACE:
            sys.exit("%s is in the schematic but not placed" % ref)

    NETNUM[""] = 0
    for i, net in enumerate(sorted(set(NETS.values())), start=1):
        NETNUM[net] = i
    pad_positions()

    body = []
    body += ['\t(net %d "%s")' % (n, name) for name, n in
             sorted(NETNUM.items(), key=lambda kv: kv[1])]
    body += outline()
    body += [footprint_block(ref) for ref in PLACE]
    tracks, segments, vias = emit_tracks(NETS)
    body += tracks
    PROBLEMS.extend(precheck(segments, vias))
    ROUTED.update(net for net, _w, _l, _p in TRACKS)

    body += [zone(["In1.Cu", "In2.Cu"], "gnd-planes", "GND"),
             zone(["F.Cu", "B.Cu"], "gnd-outer", "GND")]

    return """(kicad_pcb
\t(version 20241229)
\t(generator "toyotune gen_pcb.py")
\t(generator_version "9.0")
\t(general
\t\t(thickness 1.6)
\t\t(legacy_teardrops no)
\t)
\t(paper "A4")
\t(title_block
\t\t(title "Dash node carrier")
\t\t(company "Toyotune")
\t\t(comment 1 "GENERATED by carrier/gen_pcb.py - see carrier/schematic.txt")
\t)
\t(layers
%s
\t)
\t(setup
\t\t(pad_to_mask_clearance 0)
\t\t(allow_soldermask_bridges_in_footprints no)
\t\t(pcbplotparams
\t\t\t(layerselection 0x00010fc_ffffffff)
\t\t\t(disableapertmacros no)
\t\t\t(usegerberextensions no)
\t\t\t(usegerberattributes yes)
\t\t\t(usegerberadvancedattributes yes)
\t\t\t(creategerberjobfile yes)
\t\t\t(dashed_line_dash_ratio 12.000000)
\t\t\t(dashed_line_gap_ratio 3.000000)
\t\t\t(svgprecision 4)
\t\t\t(plotframeref no)
\t\t\t(mode 1)
\t\t\t(useauxorigin no)
\t\t\t(dxfpolygonmode yes)
\t\t\t(dxfimperialunits yes)
\t\t\t(dxfusepcbnewfont yes)
\t\t\t(psnegative no)
\t\t\t(psa4output no)
\t\t\t(plot_black_and_white yes)
\t\t\t(sketchpadsonfab no)
\t\t\t(plotpadnumbers no)
\t\t\t(hidednponfab no)
\t\t\t(sketchdnponfab yes)
\t\t\t(crossoutdnponfab yes)
\t\t\t(subtractmaskfromsilk no)
\t\t\t(outputformat 1)
\t\t\t(mirror no)
\t\t\t(drillshape 1)
\t\t\t(scaleselection 1)
\t\t\t(outputdirectory "")
\t\t)
\t)
%s
)
""" % (layer_block(), "\n".join(body))


# ---------------------------------------------------------------------------
# Checking it
# ---------------------------------------------------------------------------

def check():
    for problem in PROBLEMS:
        print("    " + problem)
    print("clearance pre-check: %d problems" % len(PROBLEMS))

    unrouted = sorted(set(NETS.values()) - ROUTED - set(["GND"]))
    if unrouted:
        print("    not routed (left for Pcbnew): %s" % " ".join(unrouted))

    build_dir = os.path.join(HERE, "build")
    report = os.path.join(build_dir, "drc.rpt")
    drc = subprocess.run([KICAD_CLI, "pcb", "drc", "--output", report,
                          "--severity-all", "--schematic-parity",
                          "--exit-code-violations", OUT],
                         capture_output=True, text=True)
    text = open(report, encoding="utf-8").read() if os.path.exists(report) else ""

    # Errors have to be zero. Warnings do not: KiCad counts silkscreen over a
    # pad as a violation, and on a board this dense some of that is the price
    # of readable part labels.
    counts, errors = {}, 0
    kind = None
    for line in text.splitlines():
        found = re.match(r"\[([a-z_]+)\]:", line)
        if found:
            kind = found.group(1)
            counts[kind] = counts.get(kind, 0) + 1
        elif kind and "error" in line:
            errors += 1
            kind = None

    for name in sorted(counts, key=lambda k: (-counts[k], k)):
        print("    %-26s %d" % (name, counts[name]))
    print("DRC: %d errors, %d warnings"
          % (errors, sum(counts.values()) - errors))
    return errors == 0 and not PROBLEMS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="run KiCad's DRC, including parity with the schematic")
    args = ap.parse_args()

    open(OUT, "w", encoding="utf-8", newline="\n").write(build())
    print("wrote carrier.kicad_pcb: %d parts on %.0f x %.0f mm, %d nets"
          % (len(PLACE), W, H, len(NETNUM) - 1))

    if args.check and not check():
        sys.exit(1)


if __name__ == "__main__":
    main()
