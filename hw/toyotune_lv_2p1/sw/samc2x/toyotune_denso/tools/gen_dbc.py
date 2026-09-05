#!/usr/bin/env python3
"""Generate toyotune.dbc from a single description of the telemetry layout.

The two Toyotune boards publish identical frames on disjoint identifier
blocks - 0x400 for CPU1, 0x420 for CPU2 - so a hand-written DBC states every
message twice.  That is precisely the kind of duplication that drifts: a
signal gets fixed in one block and forgotten in the other, and the fault shows
up much later as one board decoding differently from the other.  Describing it
once and emitting both is the cheaper arrangement.

The layout here MUST match CanTelemetry_Signals[] in can_telemetry.c.  Nothing
can enforce that from this side - the C tables are the authority - but two
things narrow the gap: the protocol version below is checked against
can_telemetry.h at generation time, and the INFO frame carries that version on
the bus so a consumer can refuse to decode a build it does not know.

Run:  python tools/gen_dbc.py -o toyotune.dbc
      python tools/gen_dbc.py --verify toyotune.dbc
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# (offset from the board's telemetry base, name, period_ms, [signals])
#
# The period is emitted as a CM_ BO_ comment, which is where can_monitor.py
# reads it from - it is metadata for humans and tooling, not something the
# firmware acts on. It must match CanTelemetry_Frames[] in can_telemetry.c.
# Each signal: (name, byte_offset, bits, signed, factor, minimum, maximum, unit, comment)
U, S = False, True

FRAMES = [
    (0, "Fast", 8, 20, [
        ("Rpm",        0, 16, U,    1, 0, 65535, "rpm", "Engine speed"),
        ("TpsRaw",     2, 16, U,    1, 0, 65535, "", "Throttle position, RAW ADC - transfer function not established"),
        ("Map",        4, 16, S,  0.1, -100, 400, "kPa", "Manifold pressure, ABSOLUTE. From Pim2, the measured value"),
        ("InjPw",      6, 16, U,    1, 0, 65535, "us", "Injector 1 pulse width"),
    ]),
    (1, "Medium1", 8, 100, [
        ("Ect",        0, 16, S, 0.01, -40, 140, "degC", "Coolant temperature"),
        ("Tha",        2, 16, S, 0.01, -40, 140, "degC", "Intake air temperature"),
        ("Tham",       4, 16, S, 0.01, -40, 140, "degC", "Manifold air temperature"),
        ("Battery",    6, 16, U, 0.01, 0, 20, "V", "Battery voltage"),
    ]),
    (2, "Medium2", 8, 100, [
        ("InjDuty",    0, 16, U, 0.01, 0, 200, "%", "Injector duty, derived from InjPw and Rpm"),
        ("KnockRetard", 2, 16, S, 0.01, 0, 30, "deg", "Current knock retard, positive magnitude"),
        ("IgnTimingRaw", 4, 8, U,   1, 0, 255, "", "Ignition timing, RAW - scaling ambiguous, see PLAN.md 3.6b"),
        ("IscvDutyRaw", 5, 8, U,    1, 0, 255, "", "Idle valve duty, RAW - transfer function not established"),
        ("LambdaRaw",  6, 8, U,     1, 0, 255, "", "Narrowband O2 ADC, RAW - not a wideband reading"),
        ("PwLoopMode", 7, 8, U,     1, 0, 255, "", "0 open loop, 200 closed loop"),
    ]),
    (5, "Medium3", 8, 100, [
        ("KnockRetardCyl1", 0, 16, S, 0.01, 0, 30, "deg", "Per-cylinder knock retard"),
        ("KnockRetardCyl2", 2, 16, S, 0.01, 0, 30, "deg", "Per-cylinder knock retard"),
        ("KnockRetardCyl3", 4, 16, S, 0.01, 0, 30, "deg", "Per-cylinder knock retard"),
        ("LambdaTrimRaw",   6,  8, U,    1, 0, 255, "", "RAW - zero point and sign not established"),
        ("MaxRetardRaw",    7,  8, U,    1, 0, 255, "", "RAW - CPU2's maximum retard"),
    ]),
    (3, "Slow", 8, 500, [
        ("NvTrimPimRaw", 0, 8, U, 1, 0, 255, "", "Learned MAP trim, RAW"),
        ("NvTrimO2Raw",  1, 8, U, 1, 0, 255, "", "Learned O2 trim, RAW"),
        ("FuelTrimRaw",  2, 8, U, 1, 0, 255, "", "RAW"),
        ("ErrorFlags1",  3, 8, U, 1, 0, 255, "", "Fault flags, bit field"),
        ("ErrorFlags2",  4, 8, U, 1, 0, 255, "", "Fault flags, bit field"),
        ("Flags46",      5, 8, U, 1, 0, 255, "", "Status flags, bit field"),
        ("Flags1",       6, 8, U, 1, 0, 255, "", "Status flags, bit field"),
        ("LimiterFlags", 7, 8, U, 1, 0, 255, "", "Limiter flags, bit field"),
    ]),
    (4, "Raw", 8, 50, [
        ("SliceIndex", 0, 8, U, 1, 0, 255, "", "Which 7-byte slice of the 72-byte DMA capture this is"),
        ("RawByte0",   1, 8, U, 1, 0, 255, "", ""),
        ("RawByte1",   2, 8, U, 1, 0, 255, "", ""),
        ("RawByte2",   3, 8, U, 1, 0, 255, "", ""),
        ("RawByte3",   4, 8, U, 1, 0, 255, "", ""),
        ("RawByte4",   5, 8, U, 1, 0, 255, "", ""),
        ("RawByte5",   6, 8, U, 1, 0, 255, "", ""),
        ("RawByte6",   7, 8, U, 1, 0, 255, "", ""),
    ]),
    (6, "Info", 8, 1000, [
        ("ProtocolVersion", 0, 8, U, 1, 0, 255, "", "Wire-protocol version; refuse to decode an unknown one"),
        ("EcuFamily",       1, 8, U, 1, 0, 255, "", "0 MR2, 1 ST205"),
        ("CpuIndex",        2, 8, U, 1, 0, 255, "", "Which Denso CPU this board is attached to"),
        ("Reserved",        3, 8, U, 1, 0, 255, "", ""),
        ("TxDropped",       4, 16, U, 1, 0, 65535, "", "Frames dropped, saturating. Non-zero means the bus is unhappy"),
        ("BusOffRecoveries", 6, 16, U, 1, 0, 65535, "", "Bus-off recoveries, saturating"),
    ]),
]

NODES = [("TT_CPU1", 0x400, "Cpu1"), ("TT_CPU2", 0x420, "Cpu2")]

VALUE_TABLES = [
    ("PwLoopMode", [(0, "OpenLoop"), (200, "ClosedLoop")]),
    ("EcuFamily", [(0, "MR2"), (1, "ST205")]),
]


def protocol_version():
    """Read the version out of can_telemetry.h so the two cannot disagree."""
    path = os.path.join(ROOT, "can_telemetry.h")
    with open(path, "r", encoding="utf-8") as f:
        m = re.search(r"#define\s+TOYOTUNE_TELEMETRY_PROTOCOL_VERSION\s*\((\d+)\)", f.read())
    if not m:
        raise SystemExit("cannot find TOYOTUNE_TELEMETRY_PROTOCOL_VERSION in can_telemetry.h")
    return int(m.group(1))


def start_bit(byte_offset, bits):
    """DBC start bit for a big-endian (Motorola) signal.

    The @0 byte order numbers bits so that the most significant bit of byte N
    is at N*8+7, which is where a Motorola signal starts.
    """
    return byte_offset * 8 + 7


def fmt_number(v):
    if isinstance(v, float) and v == int(v):
        return str(int(v))
    return str(v)


def render():
    version = protocol_version()
    out = []
    out.append('VERSION "toyotune-v2.1-proto%d"' % version)
    out.append("")
    out.append("NS_ :")
    out.append("")
    out.append("BS_:")
    out.append("")
    out.append("BU_: " + " ".join(n for n, _, _ in NODES))
    out.append("")

    comments = []

    for node, base, prefix in NODES:
        for offset, frame, length, period, signals in FRAMES:
            msg_id = base + offset
            msg_name = "TT_%s_%s" % (prefix, frame)
            out.append("BO_ %d %s: %d %s" % (msg_id, msg_name, length, node))
            for (name, byte, bits, signed, factor, lo, hi, unit, _c) in signals:
                out.append(' SG_ %s : %d|%d@0%s (%s,0) [%s|%s] "%s" Vector__XXX'
                           % (name, start_bit(byte, bits), bits,
                              "-" if signed else "+",
                              fmt_number(factor), fmt_number(lo), fmt_number(hi), unit))
            out.append("")

            comments.append('CM_ BO_ %d "Period %d ms.";' % (msg_id, period))
            for (name, _b, _bits, _s, _f, _lo, _hi, _u, comment) in signals:
                if comment:
                    comments.append('CM_ SG_ %d %s "%s";' % (msg_id, name, comment))

    out.append("")
    out.extend(comments)
    out.append("")

    for sig, pairs in VALUE_TABLES:
        for node, base, prefix in NODES:
            for offset, frame, _length, _period, signals in FRAMES:
                if any(s[0] == sig for s in signals):
                    body = " ".join('%d "%s"' % (v, n) for v, n in pairs)
                    out.append("VAL_ %d %s %s ;" % (base + offset, sig, body))
    out.append("")

    return "\n".join(out) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("-o", "--output")
    ap.add_argument("--verify", metavar="DBC")
    args = ap.parse_args()

    text = render()

    if args.verify:
        try:
            with open(args.verify, "r", encoding="utf-8", newline="") as f:
                have = f.read()
        except OSError as exc:
            print("cannot read %s: %s" % (args.verify, exc), file=sys.stderr)
            return 1
        if have.replace("\r\n", "\n") != text:
            print("%s is out of date - regenerate it" % args.verify, file=sys.stderr)
            return 1
        print("%s matches the generator" % args.verify)
        return 0

    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        print("wrote %s (protocol version %d)" % (args.output, protocol_version()))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
