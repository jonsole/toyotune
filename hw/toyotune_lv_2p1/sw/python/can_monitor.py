# Live monitor for the Toyotune v2.1 board's CAN telemetry.
#
# The decode tables are read from toyotune.dbc rather than hard-coded here.
# An earlier version of this script duplicated the frame layout in Python and
# went stale the moment the firmware's CAN design changed - it sat printing
# nothing, because it was still filtering for identifiers that were no longer
# transmitted. Reading the DBC means adding a signal to the firmware's table
# and regenerating the DBC is enough; this script follows automatically.
#
# What the board transmits (see can_telemetry.c):
#
#   0x400  fast     20 ms   RPM, TPS, PIM2, injector pulse width
#   0x401  medium  100 ms   temperatures, battery, lambda, ignition, ISCV
#   0x402  medium  100 ms   knock, and the corrections CPU2 sends back
#   0x403  slow    500 ms   learned trims, status and fault flags
#   0x404  raw      50 ms   one 7-byte slice of the full 72 DMA bytes
#
# A CPU2 board uses 0x420..0x424. Identifiers are 11-bit STANDARD - the
# earlier design used 29-bit extended ones, so a monitor written for that
# will see nothing.
#
# Signals are shown in engineering units where the DBC gives a factor (RPM,
# battery volts, injector microseconds) and as raw ECU counts otherwise.
# Coolant and intake temperature are deliberately raw: they are inverted ADC
# counts on a non-linear NTC curve, which a linear DBC factor cannot express.
# Convert with the breakpoint table in roms/docs/adc_system.md.
#
# Usage:  .venv/Scripts/python.exe can_monitor.py [--cpu2] [--raw] [--all]

import argparse
import collections
import os
import pathlib
import re
import sys
import time

BITRATE = 500000

DBC_PATH = (pathlib.Path(__file__).resolve().parent
            / ".." / "samc2x" / "toyotune_denso" / "toyotune.dbc").resolve()

# The two DMA blocks, back to back, as the raw tier sends them.
DMA_CPU1_TO_CPU2 = 38
DMA_CPU2_TO_CPU1 = 34
DMA_TOTAL = DMA_CPU1_TO_CPU2 + DMA_CPU2_TO_CPU1

Signal = collections.namedtuple("Signal", "name byte length factor offset unit")
Frame = collections.namedtuple("Frame", "id name signals period_ms")


def load_dbc(path):
    """Minimal DBC reader for the frames this project generates.

    Only handles byte-aligned Motorola signals, which is all toyotune.dbc
    contains - it is generated from the firmware's own table. It is not a
    general DBC parser and does not pretend to be.
    """
    frames, periods, current = {}, {}, None

    bo = re.compile(r"^BO_ (\d+) (\S+)\s*:\s*(\d+)")
    sg = re.compile(r'^\s*SG_ (\w+)\s*:\s*(\d+)\|(\d+)@0\+\s*'
                    r'\(([^,]+),([^)]+)\)\s*\[[^\]]*\]\s*"([^"]*)"')
    cm = re.compile(r'^CM_ BO_ (\d+) "Period (\d+) ms\.";')

    for line in path.read_text(encoding="utf-8").splitlines():
        m = bo.match(line)
        if m:
            current = Frame(int(m.group(1)), m.group(2).rstrip(":"), [], None)
            frames[current.id] = current
            continue
        m = sg.match(line)
        if m and current is not None:
            start, length = int(m.group(2)), int(m.group(3))
            if (start - 7) % 8 or length % 8:
                raise ValueError(f"{m.group(1)}: not byte aligned, unsupported here")
            current.signals.append(Signal(m.group(1), (start - 7) // 8, length,
                                          float(m.group(4)), float(m.group(5)),
                                          m.group(6)))
            continue
        m = cm.match(line)
        if m:
            periods[int(m.group(1))] = int(m.group(2))

    return {i: f._replace(period_ms=periods.get(i)) for i, f in frames.items()}


def decode(frame, data):
    out = {}
    for s in frame.signals:
        end = s.byte + s.length // 8
        if end > len(data):
            continue
        raw = int.from_bytes(data[s.byte:end], "big")
        value = raw * s.factor + s.offset
        if s.factor == 1.0 and s.offset == 0.0:
            out[s.name] = f"{raw:5d}"
        elif s.unit == "rpm":
            out[s.name] = f"{value:5.0f}"
        else:
            out[s.name] = f"{value:6.2f}{s.unit}"
    return out


class RawAssembler:
    """Rebuilds the two DMA blocks from the multiplexed raw tier."""

    def __init__(self):
        self.buffer = bytearray(DMA_TOTAL)
        self.seen = set()
        self.sweeps = 0

    def add(self, data):
        if len(data) < 2:
            return False
        slice_index = data[0]
        start = slice_index * 7
        payload = data[1:]
        if start >= DMA_TOTAL:
            return False
        count = min(len(payload), DMA_TOTAL - start)
        self.buffer[start:start + count] = payload[:count]
        self.seen.add(slice_index)
        if len(self.seen) * 7 >= DMA_TOTAL:
            self.seen.clear()
            self.sweeps += 1
            return True
        return False

    def render(self):
        a = self.buffer[:DMA_CPU1_TO_CPU2].hex(" ")
        b = self.buffer[DMA_CPU1_TO_CPU2:].hex(" ")
        return f"CPU1->CPU2 {a}\n                     CPU2->CPU1 {b}"


def main():
    parser = argparse.ArgumentParser(description="Toyotune v2.1 CAN telemetry monitor")
    parser.add_argument("--cpu2", action="store_true",
                        help="decode the CPU2 board's identifiers (0x420..)")
    parser.add_argument("--raw", action="store_true",
                        help="print the reassembled DMA blocks from the raw tier")
    parser.add_argument("--all", action="store_true",
                        help="also print frames that are not board telemetry")
    parser.add_argument("--index", type=int, default=0,
                        help="gs_usb device index when more than one is attached")
    parser.add_argument("--dbc", type=pathlib.Path, default=DBC_PATH)
    args = parser.parse_args()

    if not args.dbc.is_file():
        sys.exit(f"DBC not found: {args.dbc}")

    # Imported here rather than at module scope so the DBC parsing and decoding
    # above can be exercised without a CAN adapter or its driver stack present.
    #
    # gs_usb calls usb.backend.libusb1.get_backend() with no arguments, which
    # finds libusb-1.0.dll only if it is on PATH.  The copy bundled inside
    # libusb-package is not, so put its directory there before gs_usb loads.
    import libusb_package
    os.environ["PATH"] = (
        str(pathlib.Path(libusb_package.__file__).parent)
        + os.pathsep + os.environ["PATH"]
    )
    import can

    prefix = "TT_Cpu2_" if args.cpu2 else "TT_Cpu1_"
    frames = {i: f for i, f in load_dbc(args.dbc).items() if f.name.startswith(prefix)}
    if not frames:
        sys.exit(f"no {prefix}* frames in {args.dbc}")

    raw_id = next((i for i, f in frames.items() if f.name.endswith("_Raw")), None)
    raw = RawAssembler()

    print(f"Loaded {len(frames)} frames from {args.dbc.name}")
    for i in sorted(frames):
        f = frames[i]
        period = f"{f.period_ms} ms" if f.period_ms else "?"
        print(f"  {i:#05x} {f.name:16s} {len(f.signals)} signals, every {period}")
    print(f"Opening gs_usb device {args.index} at {BITRATE} bit/s "
          f"(11-bit standard identifiers)")

    # Deliberately NOT listen-only.  CAN_TxStandard() in can.c busy-waits on
    # TXFQS.bit.TFQF with no timeout, so an adapter that does not ACK fills the
    # 8-deep Tx queue and stalls the telemetry task.
    bus = can.Bus(interface="gs_usb", channel=args.index, index=args.index,
                  bitrate=BITRATE)

    counts = collections.Counter()
    started = time.time()
    try:
        for msg in bus:
            now = time.time() - started
            if msg.is_error_frame:
                print(f"{now:8.3f}  ERROR  id={msg.arbitration_id:03x} "
                      f"[{msg.dlc}] {msg.data.hex()}")
                continue

            frame = frames.get(msg.arbitration_id) if not msg.is_extended_id else None
            if frame is None:
                if args.all:
                    kind = "ext" if msg.is_extended_id else "std"
                    print(f"{now:8.3f}  {msg.arbitration_id:08x} {kind} "
                          f"[{msg.dlc}] {msg.data.hex(' ')}")
                continue

            counts[frame.name] += 1

            if msg.arbitration_id == raw_id:
                complete = raw.add(msg.data)
                if args.raw and complete:
                    print(f"{now:8.3f}  raw sweep {raw.sweeps}: {raw.render()}")
                continue

            rendered = "  ".join(f"{k}={v}" for k, v in decode(frame, msg.data).items())
            print(f"{now:8.3f}  {frame.name[len(prefix):]:8s} {rendered}")
    except KeyboardInterrupt:
        pass
    finally:
        bus.shutdown()
        elapsed = time.time() - started
        print()
        if not counts:
            print(f"{elapsed:.1f} s: no telemetry frames received")
            print("If the board is powered and on the bus, check the identifiers - "
                  "an older firmware used 29-bit extended 0x1001..0x1003.")
        else:
            print(f"{elapsed:.1f} s:")
            for name, count in sorted(counts.items()):
                rate = count / elapsed if elapsed else 0
                frame = next(f for f in frames.values() if f.name == name)
                expected = 1000 / frame.period_ms if frame.period_ms else None
                note = ""
                if expected:
                    note = f", expected {expected:.0f}/s"
                    if elapsed > 2 and abs(rate - expected) > expected * 0.2:
                        note += "  <-- off"
                print(f"  {name:20s} {count:6d}  {rate:6.1f}/s{note}")
            if raw.sweeps:
                print(f"  raw sweeps completed: {raw.sweeps}")


if __name__ == "__main__":
    sys.exit(main())
