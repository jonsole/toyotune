# Set the engine stimulator's RPM at runtime, over SWD, without rebuilding.
#
# The stimulator has no working console - cli.c is a leftover from another
# project and is not in the build - so VRG_SetRpm() is only ever called once,
# from main(), with a hard-coded value. But TCC0_Handler re-reads VRG_Rpm on
# every timer overflow and recomputes PERBUF from it, so simply writing the
# variable through the debugger changes the crank speed within one pattern
# slot (1.25 ms at 1000 rpm).
#
# Two details matter and are easy to get wrong by hand:
#
#   * The connection MUST be in attach mode. pyOCD's default halts the core,
#     which stops the crank signals dead and makes the ECU lose sync - and it
#     will not recover until the stimulator is running again.
#   * VRG_Rpm lives in .bss, so its address moves whenever a rebuild shifts
#     the layout. This reads the address out of the ELF every run rather than
#     trusting a constant.
#
# Usage:
#   set_rpm.py                             read the current value
#   set_rpm.py 2500                        set it
#   set_rpm.py --sweep 1000 6000 --step 500 --dwell 3
#
#   .venv/Scripts/python.exe set_rpm.py 2500

import argparse
import pathlib
import sys
import time

from elftools.elf.elffile import ELFFile
from pyocd.core.helpers import ConnectHelper

TARGET = "atsamc21j18a"
SYMBOL = "VRG_Rpm"

DEFAULT_ELF = (pathlib.Path(__file__).resolve().parent
               / ".." / "samc2x" / "stimulator" / "build" / "release" / "stimulator.elf")

# VRG_CalcPerBufValue() divides by VRG_Rpm, and the Cortex-M0+ has no hardware
# divider - __aeabi_uidiv does not fault cleanly on zero, it just yields a
# nonsense period. Keep well clear of it.
MIN_RPM = 50
MAX_RPM = 10000


def find_symbol(elf_path, name):
    """Address and size of a symbol, read from the ELF's symbol table.

    VRG_Rpm is static, so it is a local symbol - present in .symtab but not in
    .dynsym, and absent entirely from a stripped binary.
    """
    with open(elf_path, "rb") as handle:
        elf = ELFFile(handle)
        for section in elf.iter_sections():
            if section.header["sh_type"] != "SHT_SYMTAB":
                continue
            for sym in section.iter_symbols():
                if sym.name == name:
                    return sym["st_value"], sym["st_size"]
    raise SystemExit(f"{name} not found in {elf_path} - is the ELF stripped, "
                     f"or built from different sources?")


def open_target(probe):
    """Attach to the stimulator, never guessing which board when several are on.

    Writing RPM into the wrong target would at best do nothing and at worst
    scribble on the Toyotune board's RAM, so an ambiguous probe list is an
    error rather than a coin flip. blocking=False also means a missing probe
    fails immediately instead of sitting on "Waiting for a debug probe...".
    """
    probes = ConnectHelper.get_all_connected_probes(blocking=False,
                                                    print_wait_message=False)
    if not probes:
        raise SystemExit("no debug probe found. Check the Xplained Pro is "
                         "connected by its DEBUG USB port.")

    def listing():
        nl = chr(10)
        return nl.join("    " + p.unique_id + "  " + p.product_name for p in probes)

    if probe:
        match = [p for p in probes if p.unique_id == probe]
        if not match:
            raise SystemExit("no probe with id " + probe + ". Attached:"
                             + chr(10) + listing())
        chosen = match[0]
    elif len(probes) == 1:
        chosen = probes[0]
    else:
        raise SystemExit("several probes attached - pass --probe to say which "
                         "board is the stimulator:" + chr(10) + listing())

    session = ConnectHelper.session_with_chosen_probe(
        unique_id=chosen.unique_id,
        target_override=TARGET,
        blocking=False,
        options={
            "connect_mode": "attach",       # do NOT halt the running core
            "resume_on_disconnect": False,
            "warning.cortex_m_default": False,
        },
    )
    if session is None:
        raise SystemExit("could not open a session on " + chosen.unique_id)
    return session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rpm", nargs="?", type=int, help="value to write")
    parser.add_argument("--sweep", nargs=2, type=int, metavar=("FROM", "TO"),
                        help="step from FROM to TO")
    parser.add_argument("--step", type=int, default=500, help="sweep increment")
    parser.add_argument("--dwell", type=float, default=3.0,
                        help="seconds to hold each sweep point")
    parser.add_argument("--elf", default=str(DEFAULT_ELF), help="stimulator ELF")
    parser.add_argument("--probe", default=None,
                        help="probe unique id, when several are attached")
    args = parser.parse_args()

    elf = pathlib.Path(args.elf).resolve()
    if not elf.exists():
        raise SystemExit(f"ELF not found: {elf}\nBuild it with: cmake --build --preset release")

    addr, size = find_symbol(elf, SYMBOL)
    if size != 2:
        raise SystemExit(f"{SYMBOL} is {size} bytes, expected 2 (uint16_t)")
    print(f"{SYMBOL} at {addr:#010x} ({elf.name})")

    def check(value):
        if not MIN_RPM <= value <= MAX_RPM:
            raise SystemExit(f"rpm {value} out of range {MIN_RPM}..{MAX_RPM}")

    targets = []
    if args.sweep:
        lo, hi = args.sweep
        step = abs(args.step) * (1 if hi >= lo else -1)
        targets = list(range(lo, hi + (1 if step > 0 else -1), step))
        for value in targets:
            check(value)
    elif args.rpm is not None:
        check(args.rpm)
        targets = [args.rpm]

    session = open_target(args.probe)
    with session:
        target = session.target
        print(f"attached to {session.probe.product_name} "
              f"({session.probe.unique_id})")
        current = target.read16(addr)
        print(f"current: {current} rpm")

        if not targets:
            return 0

        for value in targets:
            target.write16(addr, value)
            back = target.read16(addr)
            if back != value:
                raise SystemExit(f"wrote {value} but read back {back}")
            print(f"set: {value} rpm")
            if len(targets) > 1:
                time.sleep(args.dwell)
    return 0


if __name__ == "__main__":
    sys.exit(main())
