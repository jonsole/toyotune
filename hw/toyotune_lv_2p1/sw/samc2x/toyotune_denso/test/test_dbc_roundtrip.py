#!/usr/bin/env python3
"""Decode the firmware's own frames through toyotune.dbc and check the values.

This is the only test that closes the loop between the two halves of the wire
protocol.  can_telemetry.c decides where bytes go; toyotune.dbc tells every
consumer where to find them.  They live in different files, are edited by
different hands, and nothing in either one would notice the other moving - the
symptom is a gauge reading plausibly wrong, which is the failure mode this
whole change exists to avoid.

So: run the packing test in dump mode, take the exact bytes it produced, decode
them with can_monitor.py's DBC reader, and assert the engineering values.

Run:  python test/run_tests.py   (invoked automatically)
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PYTHON_DIR = os.path.normpath(os.path.join(ROOT, "..", "..", "python"))

sys.path.insert(0, PYTHON_DIR)

DBC = os.path.join(ROOT, "toyotune.dbc")
EXE = os.path.join(HERE, "build", "can_telemetry" + (".exe" if os.name == "nt" else ""))

# The fixture the C test builds: 3000 rpm, atmospheric, 4ms injector, 82 C
# coolant, 14.6 V, 10 degrees of knock retard.  Tolerances are the conversion
# error, not slack.
EXPECTED = {
    0x400: [("Rpm", 3000, 0), ("TpsRaw", 0x1234, 0),
            ("Map", 101.3, 0.6), ("InjPw", 4000, 0)],
    0x401: [("Ect", 81.79, 0.1), ("Tha", 20.70, 0.1),
            ("Tham", 20.70, 0.1), ("Battery", 14.61, 0.05)],
    0x402: [("InjDuty", 10.0, 0.05), ("KnockRetard", 10.0, 0.01),
            ("IgnTimingRaw", 0x5A, 0), ("IscvDutyRaw", 0x40, 0),
            ("LambdaRaw", 0x77, 0), ("PwLoopMode", 0xC8, 0)],
    0x405: [("KnockRetardCyl1", 1.0, 0.01), ("KnockRetardCyl2", 2.0, 0.01),
            ("KnockRetardCyl3", 3.0, 0.01),
            ("LambdaTrimRaw", 0x33, 0), ("MaxRetardRaw", 0x44, 0)],
    0x403: [("ErrorFlags1", 0xA5, 0), ("LimiterFlags", 0x5A, 0)],
    0x406: [("ProtocolVersion", None, 0), ("CpuIndex", 1, 0),
            ("TxDropped", 7, 0), ("BusOffRecoveries", 3, 0)],
}


def raw_value(signal, data):
    """Decode one signal the way can_monitor does, but return a number."""
    end = signal.byte + signal.length // 8
    if end > len(data):
        return None
    raw = int.from_bytes(data[signal.byte:end], "big", signed=signal.signed)
    return raw * signal.factor + signal.offset


def main():
    try:
        import can_monitor
    except ImportError as exc:
        print("SKIP: cannot import can_monitor (%s)" % exc)
        return 0

    if not os.path.exists(EXE):
        print("SKIP: %s not built" % EXE)
        return 0

    dump = subprocess.run([EXE, "-d"], capture_output=True, text=True)
    if dump.returncode != 0:
        print("ERROR: dump mode failed")
        sys.stdout.write(dump.stdout)
        return 1

    frames = {}
    for line in dump.stdout.splitlines():
        m = re.match(r"FRAME (\d+)((?: [0-9A-F]{2})+)$", line.strip())
        if m:
            frames[int(m.group(1))] = bytes(
                int(b, 16) for b in m.group(2).split())

    import pathlib
    dbc = can_monitor.load_dbc(pathlib.Path(DBC))

    checks = failures = 0
    print("DBC round-trip - firmware bytes decoded through toyotune.dbc")

    for msg_id, expectations in EXPECTED.items():
        if msg_id not in frames:
            print("  FAIL 0x%03X was not emitted by the packing test" % msg_id)
            failures += 1
            continue
        if msg_id not in dbc:
            print("  FAIL 0x%03X is missing from the DBC" % msg_id)
            failures += 1
            continue

        data = frames[msg_id]
        signals = {s.name: s for s in dbc[msg_id].signals}

        for name, want, tol in expectations:
            checks += 1
            if name not in signals:
                print("  FAIL 0x%03X.%s missing from the DBC "
                      "(signed signals are skipped by an @0+-only regex)"
                      % (msg_id, name))
                failures += 1
                continue
            if want is None:
                continue
            got = raw_value(signals[name], data)
            if got is None or abs(got - want) > tol:
                print("  FAIL 0x%03X.%s decoded %s, expected %s +/- %s"
                      % (msg_id, name, got, want, tol))
                failures += 1

    # Every signal the DBC declares must actually fit the frame, or the DBC is
    # describing bytes the firmware never sends.
    for msg_id, frame in dbc.items():
        for s in frame.signals:
            checks += 1
            if s.byte + s.length // 8 > 8:
                print("  FAIL %s.%s runs past the end of an 8-byte frame"
                      % (frame.name, s.name))
                failures += 1

    print("%d checks, %d failures" % (checks, failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
