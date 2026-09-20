#!/usr/bin/env python3
"""
Generate carrier.kicad_sch - the dash node's piggyback board - from the
netlist below.

    python gen_schematic.py            writes carrier.kicad_sch
    python gen_schematic.py --erc      writes it, then runs KiCad's ERC on it

WHY A GENERATOR AND NOT A DRAWING. The design lives in one table here, next to
the reasoning in carrier/schematic.txt and PLAN.md, and the .kicad_sch is
built from it - so the file cannot drift from the decisions, and a change is a
line rather than a redraw. Open the result in KiCad and move things about as
you like; nothing here needs to run again unless the netlist changes.

HOW IT CONNECTS. Every pin gets a short stub wire and a NET LABEL, rather than
wires drawn between parts. That is electrically complete - KiCad joins nets by
name - and it is what lets the layout be generated rather than routed. It
reads like a net-list drawing, not a hand-drawn schematic; rearranging it in
KiCad is expected.

Symbols are copied out of the installed KiCad libraries, so the file carries
its own definitions and opens on a machine with a different library setup.
"""

import argparse
import os
import re
import subprocess
import sys
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "carrier.kicad_sch")

KICAD = r"C:\Program Files\KiCad\9.0"
SYMBOLS = os.path.join(KICAD, "share", "kicad", "symbols")
KICAD_CLI = os.path.join(KICAD, "bin", "kicad-cli.exe")


# ---------------------------------------------------------------------------
# THE DESIGN
#
# (reference, library symbol, value, {pin number or name: net}, footprint hint)
#
# Nets are joined by name. The ones that matter:
#   +12V_IN   the loom, fused and reverse-protected into +12V
#   +5V       the regulator's output; VBUS is +5V behind D2, with the hold-up
#   3V3       from the display module - an OUTPUT of the module, an input here
# ---------------------------------------------------------------------------

PARTS = [
    # --- the display module -------------------------------------------------
    ("J1", "Connector_Generic:Conn_01x08", "To display module",
     {"1": "IO29_NC", "2": "SDA", "3": "SCL", "4": "CAN_RXD", "5": "CAN_TXD",
      "6": "3V3", "7": "GND", "8": "VBUS"},
     "Connector_PinHeader_2.54mm:PinHeader_1x08_P2.54mm_Vertical"),

    # --- the loom, in and out: 12 V, GND, CANH, CANL straight through --------
    ("J2", "Connector_Generic:Conn_01x04", "Loom in",
     {"1": "+12V_IN", "2": "GND", "3": "CANH", "4": "CANL"},
     "Connector_JST:JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical"),
    ("J3", "Connector_Generic:Conn_01x04", "Loom out",
     {"1": "+12V_IN", "2": "GND", "3": "CANH", "4": "CANL"},
     "Connector_JST:JST_XH_B4B-XH-A_1x04_P2.50mm_Vertical"),

    # --- power in: fuse, reverse polarity, clamp ----------------------------
    ("F1", "Device:Polyfuse", "1A",
     {"1": "+12V_IN", "2": "+12V_F"}, "Fuse:Fuse_1206_3216Metric"),
    ("Q1", "Transistor_FET:Q_PMOS_GSD", "IRLML6402",
     {"G": "GATE", "S": "+12V_F", "D": "VIN"}, "Package_TO_SOT_SMD:SOT-23"),
    ("R2", "Device:R", "100k", {"1": "GATE", "2": "GND"}, "Resistor_SMD:R_0805_2012Metric"),
    # Across gate and source, cathode to the SOURCE, so it clamps how
    # negative Vgs can go on a 24 V jump start. The other way round it would
    # simply conduct and hold the FET off.
    ("D5", "Device:D_Zener", "12V",
     {"K": "+12V_F", "A": "GATE"}, "Diode_SMD:D_SOD-123"),
    ("D1", "Device:D_TVS", "SMBJ26A",
     {"1": "VIN", "2": "GND"}, "Diode_SMD:D_SMB"),
    ("C1", "Device:C", "10u/50V", {"1": "VIN", "2": "GND"}, "Capacitor_SMD:C_1206_3216Metric"),

    # --- 5 V ----------------------------------------------------------------
    ("U1", "Converter_DCDC:TSR_1-2450", "TSR 1-2450",
     {"VIN": "VIN", "GND": "GND", "VOUT": "+5V"},
     "Converter_DCDC:Converter_DCDC_TRACO_TSR-1_THT"),
    ("C2", "Device:C", "10u", {"1": "+5V", "2": "GND"}, "Capacitor_SMD:C_0805_2012Metric"),
    ("C3", "Device:C", "100n", {"1": "+5V", "2": "GND"}, "Capacitor_SMD:C_0603_1608Metric"),

    # D2 keeps the hold-up on the module's side, and stops a bench USB lead
    # and this regulator fighting over VBUS - one net on the module.
    # Pin 1 is the CATHODE on KiCad's diode symbols: current runs from the
    # regulator on pin 2 (anode) out to VBUS on pin 1.
    ("D2", "Device:D_Schottky", "SS34", {"K": "VBUS", "A": "+5V"}, "Diode_SMD:D_SMA"),
    ("C4", "Device:C_Polarized", "470u/10V", {"1": "VBUS", "2": "GND"},
     "Capacitor_SMD:CP_Elec_8x10.5"),

    # --- CAN ----------------------------------------------------------------
    ("U2", "Interface_CAN_LIN:TJA1051T-3", "TJA1051T/3",
     {"TXD": "TXD_R", "GND": "GND", "VCC": "+5V", "RXD": "CAN_RXD",
      "VIO": "3V3", "CANL": "CANL", "CANH": "CANH", "S": "GND"},
     "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    ("R1", "Device:R", "1k", {"1": "CAN_TXD", "2": "TXD_R"},
     "Resistor_SMD:R_0603_1608Metric"),
    ("C5", "Device:C", "100n", {"1": "+5V", "2": "GND"}, "Capacitor_SMD:C_0603_1608Metric"),
    ("C6", "Device:C", "100n", {"1": "3V3", "2": "GND"}, "Capacitor_SMD:C_0603_1608Metric"),

    # Split termination - FIT ONLY ON THE TWO BOARDS AT THE ENDS OF THE BUS.
    # Two 60 R with a capacitor to ground damps common mode better than one
    # 120 R; leave R3, R4 and C7 unpopulated on the middle nodes.
    ("R3", "Device:R", "60R4 (end only)", {"1": "CANH", "2": "TERM_M"},
     "Resistor_SMD:R_0805_2012Metric"),
    ("R4", "Device:R", "60R4 (end only)", {"1": "TERM_M", "2": "CANL"},
     "Resistor_SMD:R_0805_2012Metric"),
    ("C7", "Device:C", "4n7 (end only)", {"1": "TERM_M", "2": "GND"},
     "Capacitor_SMD:C_0603_1608Metric"),
    ("D3", "Device:D_TVS", "PESD2CAN", {"1": "CANH", "2": "CANL"},
     "Package_TO_SOT_SMD:SOT-23"),

    # --- the carrier's own memory -------------------------------------------
    # Node identity, calibration and the page the driver left it on: things
    # that belong to this position in the loom, not to the display module -
    # see PLAN.md 4.14a. This is why IO28 and IO27 are I2C and not the
    # identity divider and the illumination sense.
    ("U3", "Memory_EEPROM:24LC256", "24LC256",
     {"A0": "GND", "A1": "GND", "A2": "GND", "GND": "GND",
      "SDA": "SDA", "SCL": "SCL", "WP": "GND", "VCC": "3V3"},
     "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    ("R5", "Device:R", "4k7", {"1": "SDA", "2": "3V3"}, "Resistor_SMD:R_0603_1608Metric"),
    ("R6", "Device:R", "4k7", {"1": "SCL", "2": "3V3"}, "Resistor_SMD:R_0603_1608Metric"),
    ("C9", "Device:C", "100n", {"1": "3V3", "2": "GND"}, "Capacitor_SMD:C_0603_1608Metric"),

    # --- telling ERC where power comes from ---------------------------------
    # These three arrive from somewhere else - VIN from the loom through the
    # fuse and the MOSFET, GND and 3V3 from the display module - so nothing
    # on this sheet "drives" them and ERC would call each an error. The flags
    # say they are fed, and are not parts.
    ("#FLG1", "power:PWR_FLAG", "PWR_FLAG", {"1": "VIN"}, ""),
    ("#FLG2", "power:PWR_FLAG", "PWR_FLAG", {"1": "GND"}, ""),
    ("#FLG3", "power:PWR_FLAG", "PWR_FLAG", {"1": "3V3"}, ""),
]

# Pins deliberately left unconnected, so ERC says so once here rather than
# every time the schematic is opened.
NO_CONNECT = [("J1", "1")]


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


def symbol_pins(sym, by_name=False):
    """{number or name: (x, y, angle)} for unit 1, in symbol coordinates."""
    pins = {}
    for unit in find_all(sym, "symbol"):
        # Units are named "Name_<unit>_<style>"; unit 0 is common to all.
        suffix = unquote(unit[1]).rsplit("_", 2)[-2:]
        if suffix[0] not in ("0", "1"):
            continue
        for pin in find_all(unit, "pin"):
            at = next(find_all(pin, "at"))
            number = unquote(next(find_all(pin, "number"))[1])
            pname = unquote(next(find_all(pin, "name"))[1])
            key = pname if by_name else number
            pins[key] = (float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0)
            if by_name:
                pins[number] = pins[key]
    return pins


# ---------------------------------------------------------------------------
# The schematic
# ---------------------------------------------------------------------------

GRID = 2.54
STUB = 2 * GRID

# Every stub end so far, so no two land on the same point.
ENDPOINTS = set()


def uid():
    return str(uuid.uuid4())


def place(parts):
    """Left to right, top to bottom, in the order given - in columns wide
    enough for the labels."""
    # Everything on KiCad's 2.54 mm grid and well spaced: a pin that lands
    # between grid points cannot be wired to by hand afterwards, and parts
    # close enough for their label stubs to touch would short two nets
    # together - ERC caught exactly that at 60 x 45 mm.
    positions = {}
    x, y = 30.48, 30.48
    col_w, row_h = 30 * GRID, 22 * GRID		# 76.2 x 55.88 mm
    per_col = 7
    for i, part in enumerate(parts):
        positions[part[0]] = (x + (i // per_col) * col_w, y + (i % per_col) * row_h)
    return positions


def build():
    lib_symbols = {}
    body = []

    for ref, lib_id, value, nets, footprint in PARTS:
        if lib_id not in lib_symbols:
            lib_symbols[lib_id] = load_symbol(lib_id)

    positions = place(PARTS)

    for ref, lib_id, value, nets, footprint in PARTS:
        sym = lib_symbols[lib_id]
        px, py = positions[ref]
        sym_uuid = uid()

        body.append("""	(symbol
		(lib_id "%s")
		(at %.2f %.2f 0)
		(unit 1)
		(exclude_from_sim no)
		(in_bom %s)
		(on_board %s)
		(dnp no)
		(uuid "%s")
		(property "Reference" "%s" (at %.2f %.2f 0)
			(effects (font (size 1.27 1.27)) (justify left))
		)
		(property "Value" "%s" (at %.2f %.2f 0)
			(effects (font (size 1.27 1.27)) (justify left))
		)
		(property "Footprint" "%s" (at %.2f %.2f 0)
			(effects (font (size 1.27 1.27)) (hide yes))
		)
		(instances
			(project "carrier"
				(path "/%s" (reference "%s") (unit 1))
			)
		)
	)""" % (lib_id, px, py, "no" if ref.startswith("#") else "yes",
            "no" if ref.startswith("#") else "yes", sym_uuid, ref, px - 12.0, py - 6.0, value, px - 12.0, py - 3.5,
            footprint, px, py, SHEET_UUID, ref))

        pins = symbol_pins(sym, by_name=True)
        for key, net in nets.items():
            found = lookup(pins, key)
            if found is None:
                sys.exit("%s (%s): no pin %r - has %s"
                         % (ref, lib_id, key, sorted(set(pins))))
            sx, sy, angle = found
            ax, ay = px + sx, py - sy		# symbol Y is up, schematic Y is down
            # A pin's angle points from its connection point INTO the body,
            # so a stub goes the other way. Getting this backwards drew the
            # two stubs of every 2-pin part through each other.
            dx, dy = {0: (-1, 0), 90: (0, 1), 180: (1, 0), 270: (0, -1)}[int(angle) % 360]

            # Lengthen the stub until its end is nobody else's: two stubs
            # ending on one point would join their nets silently, which is
            # how +12V met VIN across the MOSFET the first time round.
            length = STUB
            while (round(ax + dx * length, 2), round(ay + dy * length, 2)) in ENDPOINTS:
                length += GRID
            ex, ey = round(ax + dx * length, 2), round(ay + dy * length, 2)
            ENDPOINTS.add((ex, ey))

            body.append("""	(wire (pts (xy %.2f %.2f) (xy %.2f %.2f))
		(stroke (width 0) (type default)) (uuid "%s")
	)""" % (ax, ay, ex, ey, uid()))

            just = "left" if dx >= 0 else "right"
            rot = 0 if dx else 90
            body.append("""	(label "%s" (at %.2f %.2f %d)
		(effects (font (size 1.27 1.27)) (justify %s bottom))
		(uuid "%s")
	)""" % (net, ex, ey, rot, just, uid()))

        for nc_ref, nc_pin in NO_CONNECT:
            if nc_ref != ref:
                continue
            sx, sy, _ = pins[nc_pin]
            body.append("""	(no_connect (at %.2f %.2f) (uuid "%s"))"""
                        % (px + sx, py - sy, uid()))

    libs = "\n".join("\t\t" + dump(sym, 3) for sym in lib_symbols.values())

    return """(kicad_sch
	(version 20250114)
	(generator "toyotune gen_schematic.py")
	(generator_version "9.0")
	(uuid "%s")
	(paper "A2")
	(title_block
		(title "Dash node carrier - 5 V, CAN and config EEPROM")
		(company "Toyotune")
		(comment 1 "GENERATED by carrier/gen_schematic.py - see carrier/schematic.txt")
		(comment 2 "Connections are by net label, not by drawn wires")
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


SHEET_UUID = uid()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--erc", action="store_true", help="run KiCad's ERC on the result")
    args = ap.parse_args()

    open(OUT, "w", encoding="utf-8", newline="\n").write(build())
    print("wrote %s: %d parts" % (os.path.relpath(OUT, HERE), len(PARTS)))

    if args.erc:
        report = os.path.join(HERE, "build", "erc.rpt")
        os.makedirs(os.path.dirname(report), exist_ok=True)
        r = subprocess.run([KICAD_CLI, "sch", "erc", "--output", report,
                            "--severity-all", "--exit-code-violations", OUT],
                           capture_output=True, text=True)
        print(r.stdout.strip() or r.stderr.strip())
        if os.path.exists(report):
            print(open(report, encoding="utf-8").read())


if __name__ == "__main__":
    main()
