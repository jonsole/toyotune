#!/usr/bin/env python3
"""
Generate carrier.kicad_sch and carrier.kicad_pro - the dash node's piggyback
board - from the design below.

    python gen_schematic.py            write the project and the schematic
    python gen_schematic.py --check    ...then ERC it, and read the netlist
                                       back to check every connection

WHY A GENERATOR RATHER THAN A DRAWING. The design lives in one place, beside
the reasoning in carrier/schematic.txt and PLAN.md, so the two cannot drift
apart, and a change is a line rather than a redraw. UUIDs are derived from
what they identify, so regenerating writes the same bytes and a diff shows
what really changed. Open the result in KiCad and move things about freely -
nothing here needs running again unless the design changes.

HOW IT IS DRAWN. Parts sit in blocks - the loom and the supply on the left,
CAN and the EEPROM in the middle, the module's connector on the right - wired
pin to pin, with POWER SYMBOLS for GND, +12V, +5V, +3V3 and VBUS. Rails are
not dragged across the sheet; that is what power symbols are for.

WHAT IS CHECKED. --check runs KiCad's own ERC and then exports the netlist and
compares it against NETS below, pin by pin, in both directions. A wire that
misses a pin by a millimetre looks perfectly fine on paper and fails here.
That check has already caught a Schottky and a gate Zener drawn backwards, and
stub wires drawn through each other.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "carrier.kicad_sch")
PROJECT = os.path.join(HERE, "carrier.kicad_pro")

KICAD = r"C:\Program Files\KiCad\9.0"
SYMBOLS = os.path.join(KICAD, "share", "kicad", "symbols")
KICAD_CLI = os.path.join(KICAD, "bin", "kicad-cli.exe")

GRID = 2.54


# ---------------------------------------------------------------------------
# THE PARTS:  ref: (symbol, value, footprint, x, y, rotation)
#
# Laid out as blocks that read left to right: the loom arrives on the left,
# through the fuse and the reverse-polarity MOSFET to the regulator; CAN and
# the EEPROM sit in the middle; the display module's connector is on the
# right. Rotation is KiCad's - counter-clockwise degrees.
# ---------------------------------------------------------------------------

PARTS = {
    # --- the loom, in and out ----------------------------------------------
    "J2": ("Connector_Generic:Conn_01x04", "Loom in",
           "Connector_Molex:Molex_Micro-Fit_3.0_43045-0400_2x02_P3.00mm_Horizontal", 38.10, 63.50, 180),
    "J3": ("Connector_Generic:Conn_01x04", "Loom out",
           "Connector_Molex:Molex_Micro-Fit_3.0_43045-0400_2x02_P3.00mm_Horizontal", 38.10, 93.98, 180),

    # --- 12 V in: fuse, reverse polarity, clamp -----------------------------
    "F1": ("Device:Polyfuse", "1A", "Fuse:Fuse_1206_3216Metric", 60.96, 66.04, 90),
    "D5": ("Device:D_Zener", "12V", "Diode_SMD:D_SOD-123", 73.66, 62.23, 90),
    "Q1": ("Transistor_FET:Q_PMOS_GSD", "IRLML6402",
           "Package_TO_SOT_SMD:SOT-23", 83.82, 63.50, 270),
    "R2": ("Device:R", "100k", "Resistor_SMD:R_0805_2012Metric", 93.98, 52.07, 0),
    "D1": ("Device:D_TVS", "SMBJ26A", "Diode_SMD:D_SMB", 99.06, 69.85, 90),
    "C1": ("Device:C", "10u/50V", "Capacitor_SMD:C_1206_3216Metric", 109.22, 73.66, 0),

    # --- 5 V -----------------------------------------------------------------
    "U1": ("Converter_DCDC:TSR_1-2450", "TSR 1-2450",
           "Converter_DCDC:Converter_DCDC_TRACO_TSR-1_THT", 120.65, 63.50, 0),
    "C2": ("Device:C", "10u", "Capacitor_SMD:C_0805_2012Metric", 139.70, 68.58, 0),
    "C3": ("Device:C", "100n", "Capacitor_SMD:C_0603_1608Metric", 149.86, 68.58, 0),
    "D2": ("Device:D_Schottky", "SS34", "Diode_SMD:D_SMA", 170.18, 60.96, 180),
    "C4": ("Device:C_Polarized", "470u/10V", "Capacitor_SMD:CP_Elec_8x10.5",
           182.88, 68.58, 0),

    # --- CAN ------------------------------------------------------------------
    "R1": ("Device:R", "1k", "Resistor_SMD:R_0603_1608Metric", 76.20, 134.62, 90),
    "U2": ("Interface_CAN_LIN:TJA1051T-3", "TJA1051T/3",
           "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", 100.33, 139.70, 0),
    "C5": ("Device:C", "100n", "Capacitor_SMD:C_0603_1608Metric", 114.30, 124.46, 0),
    "C6": ("Device:C", "100n", "Capacitor_SMD:C_0603_1608Metric", 78.74, 124.46, 0),
    "D3": ("Device:D_TVS", "PESD2CAN", "Package_TO_SOT_SMD:SOT-23", 125.73, 144.78, 90),
    "R3": ("Device:R", "60R4", "Resistor_SMD:R_0805_2012Metric",
           137.16, 140.97, 0),
    "R4": ("Device:R", "60R4", "Resistor_SMD:R_0805_2012Metric",
           137.16, 148.59, 0),
    "C7": ("Device:C", "4n7", "Capacitor_SMD:C_0603_1608Metric",
           147.32, 148.59, 0),

    # --- the carrier's own memory ---------------------------------------------
    "U3": ("Memory_EEPROM:24LC256", "24LC256",
           "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm", 100.33, 190.50, 0),
    "C9": ("Device:C", "100n", "Capacitor_SMD:C_0603_1608Metric", 71.12, 187.96, 0),
    "R5": ("Device:R", "4k7", "Resistor_SMD:R_0603_1608Metric", 123.19, 177.80, 0),
    "R6": ("Device:R", "4k7", "Resistor_SMD:R_0603_1608Metric", 133.35, 177.80, 0),

    # --- mounting -------------------------------------------------------------
    # Here so the board's holes are parts the schematic knows about: DRC's
    # parity check reports a footprint the schematic has never heard of.
    "H1": ("Mechanical:MountingHole", "M2", "MountingHole:MountingHole_2.2mm_M2", 355.60, 45.72, 0),
    "H2": ("Mechanical:MountingHole", "M2", "MountingHole:MountingHole_2.2mm_M2", 370.84, 45.72, 0),
    "H3": ("Mechanical:MountingHole", "M2", "MountingHole:MountingHole_2.2mm_M2", 386.08, 45.72, 0),
    "H4": ("Mechanical:MountingHole", "M2", "MountingHole:MountingHole_2.2mm_M2", 401.32, 45.72, 0),

    # --- the display module ---------------------------------------------------
    "J1": ("Connector_Generic:Conn_01x08", "To display module",
           "Connector_PinSocket_2.54mm:PinSocket_1x08_P2.54mm_Vertical",
           200.66, 175.26, 0),
}


# ---------------------------------------------------------------------------
# THE WIRES: a run through anchors, each "REF.PIN" or a corner (x, y).
#
# A run of three or more points is drawn as an L at every turn, so only the
# corners need listing. Where a pin already sits on a rail it is left out
# entirely: split() cuts the rail there and dots the join.
# ---------------------------------------------------------------------------

WIRES = [
    # 12 V from the loom, through the fuse, and on to the second connector
    ["J2.1", "F1.1"],
    [(48.26, 66.04), (48.26, 96.52), "J3.1"],

    # the MOSFET: source from the fuse, drain to the regulator, gate pulled
    # down through R2 and clamped to the source by D5
    ["F1.2", "Q1.S"],
    ["Q1.G", (93.98, 58.42), "R2.2"],
    ["D5.A", (83.82, 58.42)],
    ["Q1.D", "U1.Vin"],
    ["C1.1", (109.22, 66.04)],
    # D5's cathode and D1's clamp end sit ON the rails above rather than
    # stubbing onto them.

    # 5 V out, its decoupling, and VBUS behind the diode
    ["U1.Vout", "D2.A"],
    ["C2.1", (139.70, 60.96)],
    ["C3.1", (149.86, 60.96)],
    ["D2.K", (182.88, 60.96), "C4.1"],

    # CAN: the transceiver, its series resistor, the pair out through the
    # termination pads to the loom
    ["R1.2", "U2.TXD"],
    ["U2.CANH", (125.73, 137.16), (137.16, 137.16), "R3.1"],
    ["U2.CANL", (120.65, 142.24), (120.65, 152.40), (137.16, 152.40), "R4.2"],
    ["C7.1", (137.16, 144.78)],
    ["D3.A2", (125.73, 137.16)],
    ["D3.A1", (125.73, 152.40)],

    # the pair, up the left of the sheet to the two loom connectors
    [(125.73, 137.16), (125.73, 110.49), (55.88, 110.49), (55.88, 60.96), "J2.3"],
    [(55.88, 91.44), "J3.3"],
    [(120.65, 142.24), (120.65, 113.03), (53.34, 113.03), (53.34, 58.42), "J2.4"],
    [(53.34, 88.90), "J3.4"],

    # the EEPROM's two lines, up to their pull-ups and across to the module
    ["U3.SDA", (123.19, 187.96), "R5.2"],
    ["U3.SCL", (133.35, 190.50), "R6.2"],
    [(123.19, 187.96), (172.72, 187.96), (172.72, 170.18), "J1.2"],
    [(133.35, 190.50), (177.80, 190.50), (177.80, 172.72), "J1.3"],

    # CAN receive and transmit, each in its own lane under the transceiver
    ["U2.RXD", (68.58, 137.16), (68.58, 160.02), (182.88, 160.02),
     (182.88, 175.26), "J1.4"],
    ["R1.1", (63.50, 134.62), (63.50, 165.10), (187.96, 165.10),
     (187.96, 177.80), "J1.5"],
]


# Power symbols - what a schematic uses instead of drawing rails across the
# sheet. (symbol, the pin it attaches to, which way it sits)
POWER = [
    ("power:+12V", "F1.1", "up"),
    ("power:GND", "J2.2", "right"),
    ("power:GND", "J3.2", "right"),
    ("power:GND", "R2.1", "up"),
    ("power:GND", "D1.A1", "down"),
    ("power:GND", "C1.2", "down"),
    ("power:GND", "U1.GND", "down"),
    ("power:+5V", "U1.Vout", "up"),
    ("power:GND", "C2.2", "down"),
    ("power:GND", "C3.2", "down"),
    ("power:VBUS", "D2.K", "up"),
    ("power:GND", "C4.2", "down"),

    ("power:+5V", "U2.VCC", "up"),
    ("power:+5V", "C5.1", "up"),
    ("power:GND", "C5.2", "down"),
    ("power:+3V3", "U2.VIO", "left"),
    ("power:+3V3", "C6.1", "up"),
    ("power:GND", "C6.2", "down"),
    ("power:GND", "U2.GND", "down"),
    ("power:GND", "U2.S", "left"),
    ("power:GND", "C7.2", "down"),

    ("power:+3V3", "U3.VCC", "up"),
    ("power:GND", "U3.GND", "down"),
    ("power:GND", "U3.A0", "left"),
    ("power:GND", "U3.A1", "left"),
    ("power:GND", "U3.A2", "left"),
    ("power:GND", "U3.WP", "right"),
    ("power:+3V3", "C9.1", "up"),
    ("power:GND", "C9.2", "down"),
    ("power:+3V3", "R5.1", "up"),
    ("power:+3V3", "R6.1", "up"),

    ("power:VBUS", "J1.8", "left"),
    ("power:GND", "J1.7", "left"),
    ("power:+3V3", "J1.6", "left"),

    # PWR_FLAGs. Not parts: they tell ERC that a net IS driven, where nothing
    # on this sheet is a power output. Every rail here arrives from elsewhere
    # - 12 V and the ground from the loom, 3V3 out of the display module -
    # or comes through something passive, VIN through the polarity FET and
    # VBUS through D2. Without these, ERC reports five errors and stops being
    # worth reading.
    ("power:PWR_FLAG", (50.80, 66.04), "up"),      # +12V, on the loom rail
    ("power:PWR_FLAG", "C4.2", "right"),           # GND
    ("power:PWR_FLAG", (104.14, 66.04), "up"),     # VIN
    ("power:PWR_FLAG", "C4.1", "right"),           # VBUS
    ("power:PWR_FLAG", "C6.1", "left"),            # +3V3
]

# Names on the nets worth naming, each sitting on its wire.
LABELS = [
    ("12V_F", (68.58, 66.04), 0),
    ("GATE", (88.90, 58.42), 0),
    ("VIN", (101.60, 66.04), 0),
    ("CANH", (90.17, 110.49), 0),
    ("CANL", (90.17, 113.03), 0),
    ("TXD_R", (83.82, 134.62), 0),
    ("TERM_M", (140.97, 144.78), 0),
    ("CAN_RXD", (114.30, 160.02), 0),
    ("CAN_TXD", (114.30, 165.10), 0),
    ("SDA", (147.32, 187.96), 0),
    ("SCL", (147.32, 190.50), 0),
]

# Pins that are meant to go nowhere.
NO_CONNECT = [("J1", "1")]

# Text on the sheet: what each block is, and the one thing about this board
# that cannot be read off the parts - that the termination is fitted on two
# boards out of the three and which two depends on where each one sits in the
# loom. (size, in mm)
TEXTS = [
    ("12 V IN, REVERSE POLARITY, 5 V", (38.10, 44.45), 2.54),
    ("CAN", (48.26, 124.46), 2.54),
    ("CONFIG EEPROM", (48.26, 180.34), 2.54),
    ("R3, R4 and C7 are fitted ONLY on the two boards", (152.40, 130.81), 1.6),
    ("at the ends of the bus - see schematic.txt", (152.40, 134.62), 1.6),
]


# ---------------------------------------------------------------------------
# THE NETLIST THIS SHOULD PRODUCE - checked against KiCad's own by --check.
# ---------------------------------------------------------------------------

NETS = {
    "+12V": ["F1.1", "J2.1", "J3.1"],
    "12V_F": ["D5.K", "F1.2", "Q1.S"],
    "GATE": ["D5.A", "Q1.G", "R2.2"],
    "VIN": ["C1.1", "D1.A2", "Q1.D", "U1.Vin"],
    "+5V": ["C2.1", "C3.1", "C5.1", "D2.A", "U1.Vout", "U2.VCC"],
    "VBUS": ["C4.1", "D2.K", "J1.8"],
    "+3V3": ["C6.1", "C9.1", "J1.6", "R5.1", "R6.1", "U2.VIO", "U3.VCC"],
    "GND": ["C1.2", "C2.2", "C3.2", "C4.2", "C5.2", "C6.2", "C7.2", "C9.2",
            "D1.A1", "J1.7", "J2.2", "J3.2", "R2.1", "U1.GND", "U2.GND",
            "U2.S", "U3.A0", "U3.A1", "U3.A2", "U3.GND", "U3.WP"],
    "CANH": ["D3.A2", "J2.3", "J3.3", "R3.1", "U2.CANH"],
    "CANL": ["D3.A1", "J2.4", "J3.4", "R4.2", "U2.CANL"],
    "TERM_M": ["C7.1", "R3.2", "R4.1"],
    "CAN_TXD": ["J1.5", "R1.1"],
    "TXD_R": ["R1.2", "U2.TXD"],
    "CAN_RXD": ["J1.4", "U2.RXD"],
    "SDA": ["J1.2", "R5.2", "U3.SDA"],
    "SCL": ["J1.3", "R6.2", "U3.SCL"],
}


# ---------------------------------------------------------------------------
# A small s-expression reader, enough for KiCad's symbol libraries
# ---------------------------------------------------------------------------

TOKEN = re.compile(r'\s*(?:([()])|"((?:[^"\\]|\\.)*)"|([^\s()]+))')


def parse(text, pos=0):
    """Returns (node, pos). A node is a list, a quoted string ('"...'), or a
    bare token."""
    out = []
    while pos < len(text):
        m = TOKEN.match(text, pos)
        if m is None:
            break
        pos = m.end()
        if m.group(1) == "(":
            node, pos = parse(text, pos)
            out.append(node)
        elif m.group(1) == ")":
            return out, pos
        elif m.group(2) is not None:
            out.append('"' + m.group(2))
        else:
            out.append(m.group(3))
    return out, pos


def head(node):
    return node[0] if node and isinstance(node[0], str) else None


def find_all(node, name):
    for child in node:
        if isinstance(child, list) and head(child) == name:
            yield child


def unquote(s):
    return s[1:] if isinstance(s, str) and s.startswith('"') else s


def dump(node, indent=1):
    """Back to text, close enough to KiCad's own formatting to be readable."""
    if isinstance(node, str):
        if node.startswith('"'):
            return '"%s"' % node[1:]
        return node
    pad = "\t" * indent
    parts = [dump(c, indent + 1) for c in node]
    one = "(" + " ".join(parts) + ")"
    if len(one) < 100 and not any(isinstance(c, list) for c in node[1:]):
        return one
    out = "(" + parts[0]
    for child, text in zip(node[1:], parts[1:]):
        out += "\n" + pad + text if isinstance(child, list) else " " + text
    return out + "\n" + "\t" * (indent - 1) + ")"


# ---------------------------------------------------------------------------
# Symbols out of the installed libraries
# ---------------------------------------------------------------------------

def load_symbol(lib_id):
    """The symbol, ready to embed: named "Lib:Name" as a schematic wants, and
    with anything it inherits folded in.

    A derived symbol - 24LC256 "extends" 24LC16 - carries only its properties
    and none of the geometry or pins, so the parent's body is copied in and
    the child's properties win."""
    lib, name = lib_id.split(":", 1)
    path = os.path.join(SYMBOLS, lib + ".kicad_sym")
    if not os.path.exists(path):
        sys.exit("no such library: %s" % path)

    tree, _ = parse(open(path, encoding="utf-8").read())
    by_name = {unquote(sym[1]): sym for sym in find_all(tree[0], "symbol")}
    if name not in by_name:
        sys.exit("%s not found in %s" % (name, lib))

    sym = by_name[name]
    parent_name = None
    for node in find_all(sym, "extends"):
        parent_name = unquote(node[1])

    if parent_name is not None:
        if parent_name not in by_name:
            sys.exit("%s extends %s, which is not in %s" % (name, parent_name, lib))
        parent = by_name[parent_name]
        props = {unquote(n[1]): n for n in find_all(parent, "property")}
        props.update({unquote(n[1]): n for n in find_all(sym, "property")})
        merged = ["symbol", '"' + name]
        merged += [n for n in parent[2:]
                   if not (isinstance(n, list) and head(n) == "property")]
        merged += list(props.values())
        sym = merged

    # Rename the unit sub-symbols to match, and the symbol itself to the
    # library id a schematic refers to it by.
    out = ["symbol", '"' + lib_id]
    for node in sym[2:]:
        if isinstance(node, list) and head(node) == "symbol":
            unit = unquote(node[1]).rsplit("_", 2)[-2:]
            node = ["symbol", '"' + name + "_" + "_".join(unit)] + node[2:]
        if isinstance(node, list) and head(node) == "extends":
            continue
        out.append(node)
    return out


# The libraries are not consistent about pin names - Vin against VIN, VSS
# against GND - so a name in the table above is matched loosely rather than
# every part being looked up by hand.
ALIASES = {"gnd": ("vss", "vee", "gnd"), "vcc": ("vdd", "vcc", "vddio"),
           "vin": ("vin", "in", "+vin"), "vout": ("vout", "out", "+vout")}


def lookup(pins, key):
    if key in pins:
        return pins[key]
    lower = {k.lower(): v for k, v in pins.items()}
    if key.lower() in lower:
        return lower[key.lower()]
    for names in ALIASES.values():
        if key.lower() in names:
            for name in names:
                if name in lower:
                    return lower[name]
    return None


def symbol_pins(sym):
    """{number and name: (x, y, angle)} in symbol space, where y runs up."""
    pins = {}
    for unit in find_all(sym, "symbol"):
        # Units are named "Name_<unit>_<style>"; unit 0 is common to all.
        if unquote(unit[1]).rsplit("_", 2)[-2] not in ("0", "1"):
            continue
        for pin in find_all(unit, "pin"):
            at = next(find_all(pin, "at"))
            number = unquote(next(find_all(pin, "number"))[1])
            name = unquote(next(find_all(pin, "name"))[1])
            where = (float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0)
            pins[number] = where
            if name not in ("~", ""):
                pins[name] = where
    return pins



# ---------------------------------------------------------------------------
# Where every pin ends up on the sheet
# ---------------------------------------------------------------------------

def rotate(dx, dy, angle):
    """KiCad rotates counter-clockwise, and the sheet's y runs downwards."""
    angle = int(angle) % 360
    if angle == 0:
        return dx, dy
    if angle == 90:
        return dy, -dx
    if angle == 180:
        return -dx, -dy
    return -dy, dx


SYMS = {}
PINPOS = {}


def symbol_of(lib_id):
    if lib_id not in SYMS:
        SYMS[lib_id] = load_symbol(lib_id)
    return SYMS[lib_id]


def compute_pins():
    for ref, (lib_id, _value, _fp, px, py, rot) in PARTS.items():
        for key, (sx, sy, _angle) in symbol_pins(symbol_of(lib_id)).items():
            dx, dy = rotate(sx, -sy, rot)	# symbol y is up, the sheet's is down
            PINPOS["%s.%s" % (ref, key)] = (round(px + dx, 2), round(py + dy, 2))


def anchor(a):
    if isinstance(a, tuple):
        return (round(a[0], 2), round(a[1], 2))
    if a not in PINPOS:
        sys.exit("no such pin: %s" % a)
    return PINPOS[a]


def route(points):
    """Anchors to segments. Two points sharing neither x nor y are cornered -
    across, then down."""
    out = []
    pts = [anchor(p) for p in points]
    for a, b in zip(pts, pts[1:]):
        if abs(a[0] - b[0]) < 0.01 or abs(a[1] - b[1]) < 0.01:
            out.append((a, b))
        else:
            corner = (b[0], a[1])
            out += [(a, corner), (corner, b)]
    return [seg for seg in out if seg[0] != seg[1]]


def on_segment(p, seg):
    """Is p strictly inside seg? Every wire here is horizontal or vertical."""
    (ax, ay), (bx, by) = seg
    if abs(ax - bx) < 0.01:
        return abs(p[0] - ax) < 0.01 and min(ay, by) < p[1] < max(ay, by)
    if abs(ay - by) < 0.01:
        return abs(p[1] - ay) < 0.01 and min(ax, bx) < p[0] < max(ax, bx)
    return False


def split(segments):
    """Cut every wire where something taps it part way along - another wire's
    end, or a pin sitting on it.

    KiCad connects wires end to end, and honours only the FIRST tap part way
    along a wire however many junction dots are drawn on it. A sheet written
    with one long rail and four stubs therefore reads back with three of the
    four silently unconnected. Splitting the rail into one wire per span makes
    every join end to end, which is what KiCad itself stores when the same
    thing is drawn by hand."""
    taps = set(p for seg in segments for p in seg) | set(PINPOS.values())

    out = []
    for seg in segments:
        cuts = sorted(p for p in taps if on_segment(p, seg))
        if seg[0] > seg[1]:
            cuts.reverse()
        here = seg[0]
        for p in cuts + [seg[1]]:
            out.append((here, p))
            here = p
    return out


def junctions(segments):
    """Where a dot is needed. Everything here meets end to end after split(),
    so it comes down to counting: two ends meeting are a corner and want no
    dot, three or more do - and a pin counts as one of them, which is the dot
    on a part that sits part way along a rail."""
    counts = {}
    for seg in segments:
        for p in seg:
            counts[p] = counts.get(p, 0) + 1

    pins = set(PINPOS.values())
    return sorted(p for p, n in counts.items()
                  if n + (1 if p in pins else 0) >= 3)


# ---------------------------------------------------------------------------
# Writing it out
# ---------------------------------------------------------------------------

NAMESPACE = uuid.UUID("6f1d4b3a-0000-4000-8000-746f796f7475")


def uid(key):
    return str(uuid.uuid5(NAMESPACE, key))


SHEET_UUID = uid("sheet:root")

STEP = {"up": (0.0, -GRID), "down": (0.0, GRID),
        "left": (-GRID, 0.0), "right": (GRID, 0.0)}

# A rail symbol's graphic points up at rotation 0; a ground's points down. So
# each is turned to face away from the pin it hangs off.
RAIL_ROT = {"up": 0, "down": 180, "left": 90, "right": 270}
GND_ROT = {"down": 0, "up": 180, "left": 270, "right": 90}


def extent(lib_id, rot):
    """How far the drawn body reaches above, below and to the right of its
    origin, so the reference and the value sit clear of it rather than across
    it."""
    xs, ys = [], []
    for unit in find_all(load_symbol(lib_id), "symbol"):
        for kind in ("rectangle", "polyline", "circle"):
            for shape in find_all(unit, kind):
                for key in ("start", "end", "center", "xy"):
                    for at in find_all(shape, key):
                        dx, dy = rotate(float(at[1]), -float(at[2]), rot)
                        xs.append(dx)
                        ys.append(dy)
    if not ys:
        return 1.27, 1.27, 1.27
    return max(-min(ys), 1.27), max(max(ys), 1.27), max(max(xs), 1.27)


def fields_at(lib_id, x, y, rot):
    """Where the reference and the value go. Above and below a part that is
    drawn horizontally; beside one drawn vertically, where above and below is
    where its own wires are."""
    above, below, right = extent(lib_id, rot)
    if rot in (90, 270):
        return (x + right + 1.27, y - 1.27), (x + right + 1.27, y + 1.27)
    return (x + 1.27, y - above - 1.27), (x + 1.27, y + below + 2.54)


VALUE_AT = {"up": (0.0, -3.81, "left"), "down": (0.0, 4.32, "left"),
            "left": (-3.81, 0.0, "right"), "right": (3.81, 0.0, "left")}


def symbol_block(ref, lib_id, value, footprint, x, y, rot, where=None):
    """One placed symbol. A power symbol is passed `where` - the direction it
    faces - which both hides its #PWR reference and puts its NAME clear of the
    graphic on the far side from the pin. That name is the only thing on the
    sheet that says which rail a stub is, so it is never hidden: an earlier
    version hid the reference and the value together and left 39 anonymous
    stubs behind."""
    power = where is not None
    flag = lib_id.endswith("PWR_FLAG")
    ref_at, value_at = fields_at(lib_id, x, y, rot)
    justify = "left"
    if power:
        dx, dy, justify = VALUE_AT[where]
        value_at = (x + dx, y + dy)
    return """	(symbol
		(lib_id "%s")
		(at %.2f %.2f %d)
		(unit 1)
		(exclude_from_sim no)
		(in_bom %s)
		(on_board %s)
		(dnp no)
		(uuid "%s")
		(property "Reference" "%s"
			(at %.2f %.2f 0)
			(effects (font (size 1.27 1.27)) (justify left) %s)
		)
		(property "Value" "%s"
			(at %.2f %.2f 0)
			(effects (font (size 1.27 1.27)) (justify %s) %s)
		)
		(property "Footprint" "%s"
			(at %.2f %.2f 0)
			(effects (font (size 1.27 1.27)) (hide yes))
		)
		(instances
			(project "carrier"
				(path "/%s" (reference "%s") (unit 1))
			)
		)
	)""" % (lib_id, x, y, rot,
         "no" if power or ref.startswith("H") else "yes",
         "no" if power else "yes",
         uid("symbol:" + ref), ref, ref_at[0], ref_at[1],
         "(hide yes)" if power else "",
         value, value_at[0], value_at[1], justify,
         "(hide yes)" if flag else "", footprint, x, y,
         SHEET_UUID, ref)


def build():
    compute_pins()
    body = [symbol_block(ref, lib, value, fp, x, y, rot)
            for ref, (lib, value, fp, x, y, rot) in PARTS.items()]

    segments = []
    for run in WIRES:
        segments += route(run)

    for i, (lib_id, pin, where) in enumerate(POWER):
        symbol_of(lib_id)
        px, py = anchor(pin)
        dx, dy = STEP[where]
        at = (round(px + dx, 2), round(py + dy, 2))
        rot = (GND_ROT if lib_id.endswith("GND") else RAIL_ROT)[where]
        prefix = "#FLG" if lib_id.endswith("PWR_FLAG") else "#PWR"
        body.append(symbol_block("%s%02d" % (prefix, i + 1), lib_id,
                                 lib_id.split(":")[1], "", at[0], at[1], rot,
                                 where=where))
        segments.append(((px, py), at))

    segments = split(segments)

    for p in junctions(segments):
        body.append("""	(junction (at %.2f %.2f) (diameter 0) (color 0 0 0 0)
		(uuid "%s")
	)""" % (p[0], p[1], uid("junction:%.2f:%.2f" % p)))

    for a, b in segments:
        body.append("""	(wire (pts (xy %.2f %.2f) (xy %.2f %.2f))
		(stroke (width 0) (type default))
		(uuid "%s")
	)""" % (a[0], a[1], b[0], b[1], uid("wire:%.2f:%.2f:%.2f:%.2f" % (a + b))))

    for name, at, rot in LABELS:
        body.append("""	(label "%s"
		(at %.2f %.2f %d)
		(effects (font (size 1.27 1.27)) (justify left bottom))
		(uuid "%s")
	)""" % (name, at[0], at[1], rot, uid("label:" + name)))

    for text, at, size in TEXTS:
        body.append("""	(text "%s"
		(exclude_from_sim no)
		(at %.2f %.2f 0)
		(effects (font (size %.2f %.2f)) (justify left bottom))
		(uuid "%s")
	)""" % (text, at[0], at[1], size, size, uid("text:" + text)))

    for ref, pin in NO_CONNECT:
        x, y = anchor("%s.%s" % (ref, pin))
        body.append("""	(no_connect (at %.2f %.2f) (uuid "%s"))"""
                    % (x, y, uid("nc:%s:%s" % (ref, pin))))

    libs = "\n".join("\t\t" + dump(SYMS[k], 3) for k in sorted(SYMS))

    return """(kicad_sch
	(version 20250114)
	(generator "toyotune gen_schematic.py")
	(generator_version "9.0")
	(uuid "%s")
	(paper "A3")
	(title_block
		(title "Dash node carrier - 5 V, CAN and config EEPROM")
		(company "Toyotune")
		(comment 1 "GENERATED by carrier/gen_schematic.py - see carrier/schematic.txt")
	)
	(lib_symbols
%s
	)
%s
	(sheet_instances
		(path "/" (page "1"))
	)
	(embedded_fonts no)
)
""" % (SHEET_UUID, libs, "\n".join(body))


def write_project():
    """Minimal on purpose: KiCad fills in what it does not find, and a file of
    defaults copied from a demo would bury the few lines that are ours. The
    root sheet's uuid has to match the schematic's."""
    project = {
        "meta": {"filename": "carrier.kicad_pro", "version": 3},
        "sheets": [[SHEET_UUID, "Root"]],
        "libraries": {"pinned_footprint_libs": [], "pinned_symbol_libs": []},
        "text_variables": {},
        "board": {},
        "boards": [],
        "cvpcb": {"equivalence_files": []},
        "erc": {},
        "net_settings": {"classes": [{"name": "Default", "clearance": 0.2,
                                      "track_width": 0.25, "via_diameter": 0.8,
                                      "via_drill": 0.4}]},
        "pcbnew": {"page_layout_descr_file": ""},
        "schematic": {"legacy_lib_dir": "", "legacy_lib_list": []},
    }
    with open(PROJECT, "w", encoding="utf-8", newline="\n") as f:
        json.dump(project, f, indent=2)
        f.write("\n")


# ---------------------------------------------------------------------------
# Checking it against what it was meant to be
# ---------------------------------------------------------------------------

def pin_number(ref, key):
    """The design names pins as the symbol does - "Vout", "CANH", "2". KiCad's
    netlist gives numbers, so compare on those."""
    pins = symbol_pins(symbol_of(PARTS[ref][0]))
    where = lookup(pins, key)
    if where is None:
        sys.exit("%s has no pin %s" % (ref, key))
    return next(n for n, v in pins.items() if v == where and n.isdigit())


def check():
    build_dir = os.path.join(HERE, "build")
    os.makedirs(build_dir, exist_ok=True)

    report = os.path.join(build_dir, "erc.rpt")
    erc = subprocess.run([KICAD_CLI, "sch", "erc", "--output", report,
                          "--severity-all", "--exit-code-violations", OUT],
                         capture_output=True, text=True)
    print((erc.stdout or erc.stderr).strip().splitlines()[-1])

    net_file = os.path.join(build_dir, "carrier.net")
    subprocess.run([KICAD_CLI, "sch", "export", "netlist", "--format", "kicadsexpr",
                    "--output", net_file, OUT], capture_output=True, text=True)

    got = {}
    text = open(net_file, encoding="utf-8").read()
    net = None
    for line in text.splitlines():
        named = re.search(r'\(net \(code "\d+"\) \(name "([^"]+)"\)', line)
        if named:
            net = named.group(1).lstrip("/")
            got[net] = set()
        node = re.search(r'\(node \(ref "([^"]+)"\) \(pin "([^"]+)"\)', line)
        if node and net:
            got[net].add("%s.%s" % node.groups())

    want = {name: set("%s.%s" % (p.split(".")[0], pin_number(*p.split(".")))
                      for p in pins)
            for name, pins in NETS.items()}

    # A no-connect pin sits in a net of its own that the design does not name.
    nc = set("%s.%s" % (ref, pin) for ref, pin in NO_CONNECT)
    got = dict((name, pins) for name, pins in got.items() if pins - nc)

    ok = True
    for name in sorted(set(want) | set(got)):
        w, g = want.get(name, set()), got.get(name, set())
        if w == g:
            continue
        ok = False
        print("net %s" % name)
        if w - g:
            print("    missing:   %s" % " ".join(sorted(w - g)))
        if g - w:
            print("    unexpected: %s" % " ".join(sorted(g - w)))

    print("netlist matches the design" if ok else "NETLIST DOES NOT MATCH THE DESIGN")
    return ok and erc.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="run ERC, then compare KiCad's netlist with the design")
    args = ap.parse_args()

    open(OUT, "w", encoding="utf-8", newline="\n").write(build())
    write_project()
    print("wrote carrier.kicad_sch and carrier.kicad_pro: %d parts, %d power symbols"
          % (len(PARTS), len(POWER)))

    if args.check and not check():
        sys.exit(1)


if __name__ == "__main__":
    main()
