#!/usr/bin/env python3
"""
Play engine telemetry onto the bus from the CANable, as if a Toyotune board
were sending it - to drive the dash gauges on a bench with no ECU turning.

    python can_drive.py                       a rev sweep, idle to 7000 and back
    python can_drive.py --rpm 3500 --map 150  fixed values (kPa absolute)
    python can_drive.py --fault               the sweep with a stored fault set

The frames, their layouts and their periods are read from toyotune.dbc through
can_monitor's own parser, so this cannot drift from what the SAMC21 sends or
what the dash decodes: a signal added to the DBC is encoded here without a
change to this file. Values are given in engineering units - rpm, kPa, degC -
and turned into raw counts by the DBC's own factor.

WHAT THIS CANNOT DO. The dash's mixture and exhaust temperature come from the
Spartan 3 wideband, whose frames are not decoded yet (PLAN.md 4.9), so those
two gauges stay at "--" whatever is sent here.

TWO THINGS THAT MATTER ON A REAL BUS
- It refuses to start if a Toyotune board is already sending on the same
  identifiers. Two transmitters on one identifier cannot be separated by
  arbitration: both send, both see bit errors, both retry - and the real
  board's telemetry is corrupted along with this.
- The dash must be built with -DDASH_SIMULATE=OFF. A simulate build writes its
  own synthetic values over these every 20 ms.

Needs this directory's .venv (python-can, gs_usb, libusb-package). Ctrl+C to
stop; the adapter is always shut down, so it does not go on ACKing on its own.
"""

import argparse
import math
import os
import pathlib
import sys
import time

import libusb_package

os.environ["PATH"] = (str(pathlib.Path(libusb_package.__file__).parent)
                      + os.pathsep + os.environ["PATH"])
import can  # noqa: E402 - must follow the PATH fix, see the canable skill

from can_monitor import DBC_PATH, load_dbc  # noqa: E402

BITRATE = 500000

# Periods for any frame whose DBC comment does not give one.
DEFAULT_PERIOD_MS = {"Fast": 20, "Medium1": 100, "Medium2": 100, "Medium3": 100,
                     "Slow": 500, "Info": 1000}

# What the dash checks the Info frame for - DASH_EXPECTED_PROTOCOL_VERSION in
# its signals.h. A mismatch makes it refuse to decode anything, deliberately.
PROTOCOL_VERSION = 1

ATMOSPHERE_KPA = 101.3


# ---------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------

def sweep(t, cycle_s, fault):
    """A turbo engine revving from idle to near the limiter and back: vacuum
    at idle, atmospheric by 3000 rpm, boost building to 1.3 bar at the top,
    and the charge in the manifold warming as the boost builds."""
    half = cycle_s / 2.0
    phase = t % cycle_s
    x = phase / half if phase < half else (cycle_s - phase) / half   # 0..1..0
    rpm = 800 + x * (7000 - 800)

    if rpm <= 3000:
        map_kpa = 35 + (rpm - 800) * (ATMOSPHERE_KPA - 35) / (3000 - 800)
    else:
        map_kpa = ATMOSPHERE_KPA + (rpm - 3000) * (230 - ATMOSPHERE_KPA) / (7000 - 3000)
    boost = max(0.0, (map_kpa - ATMOSPHERE_KPA) / 100.0)                  # bar

    return {
        "Rpm": rpm,
        "Map": map_kpa,
        "TpsRaw": 100 + x * 800,
        "InjPw": 2000 + x * 9000,
        "Ect": 88.0,
        "Tha": 18.0,
        "Tham": 25.0 + boost * 30.0,
        "Battery": 13.9,
        "InjDuty": x * 85.0,
        "KnockRetard": 0.0,
        "ErrorFlags1": 0x04 if fault else 0,
    }


def step(t, hold_s, fault):
    """Exact values held long enough to read against the face's marks, for
    checking accuracy rather than for looking lively: rpm 1000, 2000 ... 8000,
    one per hold, then round again; boost alongside it in half-bar steps from
    -1.0 to +1.5 bar, which are the boost face's own labelled ticks; and the
    air temperatures on theirs, -20 to 100 C in 20s - the intake climbing
    while the manifold falls, so it is plain which needle is which."""
    n = int(t // hold_s)
    v = sweep(0.0, 12.0, fault)
    v["Rpm"] = 1000.0 * (1 + n % 8)
    v["Map"] = ATMOSPHERE_KPA + 100.0 * (-1.0 + 0.5 * (n % 6))
    v["Tha"] = -20.0 + 20.0 * (n % 7)
    v["Tham"] = 100.0 - 20.0 * (n % 7)
    return v


def fixed(args):
    v = sweep(0.0, 12.0, args.fault)
    for name, value in (("Rpm", args.rpm), ("Map", args.map), ("Ect", args.ect),
                        ("Tha", args.tha), ("Tham", args.tham)):
        if value is not None:
            v[name] = value
    return v


# ---------------------------------------------------------------------------
# Encoding, from the DBC
# ---------------------------------------------------------------------------

def encode(frame, values):
    """Engineering values to an 8-byte payload. Anything the frame carries
    that is not given is sent as zero - which for the flag bytes is exactly
    "no fault", and for everything else is a plain reading of zero."""
    data = bytearray(8)
    for s in frame.signals:
        value = values.get(s.name, 0.0)
        raw = int(round((value - s.offset) / s.factor))
        bits = s.length
        lo, hi = ((-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if s.signed
                  else (0, (1 << bits) - 1))
        raw = max(lo, min(hi, raw))
        data[s.byte:s.byte + bits // 8] = raw.to_bytes(bits // 8, "big", signed=s.signed)
    return bytes(data)


def open_bus():
    return can.Bus(interface="gs_usb", channel=0, index=0, bitrate=BITRATE)


def someone_else_sending(bus, ids, window_s=1.0):
    """Listen before talking: any of our identifiers already on the bus means
    a real board is there.

    The adapter hands over whatever it buffered before this session opened -
    including echoes of a previous run's own frames - all at once, so drain
    that first. Without it, the last run's backlog reads as a second sender
    and this refuses to start against a bus nobody else is on."""
    drain_end = time.time() + 2.0
    while time.time() < drain_end and bus.recv(timeout=0.2) is not None:
        pass

    end = time.time() + window_s
    while time.time() < end:
        m = bus.recv(timeout=0.1)
        if m is not None and not m.is_error_frame and m.arbitration_id in ids:
            return m.arbitration_id
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--cpu", type=int, choices=(1, 2), default=1,
                    help="whose identifiers to send: CPU1 0x400.. (default) or CPU2 0x420..")
    ap.add_argument("--cycle", type=float, default=12.0,
                    help="seconds for one sweep, idle to the top and back (default 12)")
    ap.add_argument("--rpm", type=float, help="fixed engine speed, rpm")
    ap.add_argument("--map", type=float, help="fixed manifold pressure, kPa ABSOLUTE (101.3 is 0 bar)")
    ap.add_argument("--ect", type=float, help="fixed coolant temperature, degC")
    ap.add_argument("--tha", type=float, help="fixed intake air temperature, degC")
    ap.add_argument("--tham", type=float, help="fixed manifold air temperature, degC")
    ap.add_argument("--step", type=float, nargs="?", const=1.0, default=None, metavar="SECONDS",
                    help="step instead of sweeping: rpm 1000..8000 and boost -1.0..+1.5 bar, "
                         "each value held SECONDS (default 1) - for reading accuracy off the face")
    ap.add_argument("--fault", action="store_true",
                    help="set a stored-fault flag - the dash's warning takeover and its chime")
    ap.add_argument("--seconds", type=float, default=0.0,
                    help="stop after this long (default: run until Ctrl+C)")
    ap.add_argument("--verbose", action="store_true",
                    help="one status line per second instead of one updated in place")
    args = ap.parse_args()

    prefix = "TT_Cpu%d_" % args.cpu
    frames = [f for f in load_dbc(DBC_PATH).values()
              if f.name.startswith(prefix) and not f.name.endswith("_Raw")]
    if not frames:
        sys.exit("no %s* frames in %s" % (prefix, DBC_PATH))

    schedule = []
    for f in frames:
        tier = f.name[len(prefix):]
        period = f.period_ms or DEFAULT_PERIOD_MS.get(tier, 100)
        schedule.append([f, period / 1000.0, 0.0])

    fixed_mode = any(v is not None for v in (args.rpm, args.map, args.ect, args.tha, args.tham))

    bus = open_bus()
    try:
        clash = someone_else_sending(bus, {f.id for f in frames})
        if clash is not None:
            sys.exit("0x%03X is already on the bus - a Toyotune board is sending. "
                     "Refusing to collide with it; unplug it or use --cpu %d."
                     % (clash, 3 - args.cpu))

        print("sending %s from %s - %s. Ctrl+C to stop."
              % (", ".join("0x%03X %s every %d ms" % (f.id, f.name[len(prefix):], int(p * 1000))
                           for f, p, _ in sorted(schedule, key=lambda e: e[0].id)),
                 DBC_PATH.name,
                 "fixed values" if fixed_mode
                 else ("steps held %.1f s" % args.step if args.step is not None
                       else "a %.0f s rev sweep" % args.cycle)))
        if args.fault:
            print("ErrorFlags1 = 0x04: the dash should take over with its warning page")

        t0 = time.monotonic()
        sent = errors = 0
        heartbeats = bus_errors = 0
        last_error = ""
        last_report = t0
        while True:
            now = time.monotonic()
            t = now - t0
            if args.seconds and t >= args.seconds:
                break

            if fixed_mode:
                values = fixed(args)
            elif args.step is not None:
                values = step(t, args.step, args.fault)
            else:
                values = sweep(t, args.cycle, args.fault)
            values["ProtocolVersion"] = PROTOCOL_VERSION
            values["CpuIndex"] = args.cpu - 1

            for entry in schedule:
                f, period, due = entry
                if now < due:
                    continue
                entry[2] = now + period if due == 0.0 else due + period
                if entry[2] < now:          # fell behind: do not burst to catch up
                    entry[2] = now + period
                msg = can.Message(arbitration_id=f.id, data=encode(f, values),
                                  is_extended_id=False)
                try:
                    bus.send(msg, timeout=0.05)
                    sent += 1
                except can.CanError:
                    errors += 1

            # What the adapter sees coming back: error reports, and the dash
            # nodes' heartbeats. A bus that is failing shows here long before
            # it shows on a gauge.
            while True:
                m = bus.recv(timeout=0.0)
                if m is None:
                    break
                if m.is_error_frame:
                    bus_errors += 1
                    last_error = m.data.hex(" ")
                elif 0x440 <= m.arbitration_id <= 0x443:
                    heartbeats += 1

            if now - last_report >= 1.0:
                last_report = now
                print("\r  %6.1f s  rpm %5.0f  map %5.1f kPa  tham %4.1f C  sent %d  send errors %d"
                      "  | heard: heartbeats %d  bus errors %d%s   "
                      % (t, values["Rpm"], values["Map"], values["Tham"], sent, errors,
                         heartbeats, bus_errors,
                         ("  last " + last_error) if last_error else ""),
                      end="" if not args.verbose else "\n", flush=True)

            next_due = min(e[2] for e in schedule)
            time.sleep(max(0.0, min(0.005, next_due - time.monotonic())))
    except KeyboardInterrupt:
        pass
    finally:
        bus.shutdown()
        print("\nstopped; adapter shut down")


if __name__ == "__main__":
    main()
