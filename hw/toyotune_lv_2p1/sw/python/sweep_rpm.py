# Sweep the engine stimulator's RPM up and down continuously, over ONE debug
# session.
#
# set_rpm.py attaches, writes, detaches - fine for one value, but a sweep made
# of separate runs spends most of its time re-attaching, and each run is a
# fresh chance for the probe to be busy. This attaches once and stays.
#
# IT CHECKS THE HARDWARE, NOT JUST THE VARIABLE. Writing VRG_Rpm and reading it
# back only proves a word of RAM changed. What sets the crank speed is TCC0's
# period register, which TCC0_Handler reloads from VRG_Rpm on each overflow -
# so every step here also reads TCC0->PER and says whether it moved the way
# the new speed says it should. If the variable changes and the period does
# not, the write is landing somewhere the running firmware is not reading:
# the ELF does not match what is flashed, or the handler is not running.
#
# Usage:
#   sweep_rpm.py                                  900..3000..900, forever
#   sweep_rpm.py --low 900 --high 6000 --step 300 --dwell 1
#   sweep_rpm.py --cycles 1                       once up and back, then stop
#   sweep_rpm.py --hold 2500                      set one value, check, leave
#
#   .venv/Scripts/python.exe sweep_rpm.py
#
# Ctrl+C stops it where it is and leaves the stimulator running at that speed.

import argparse
import sys
import time

from pyocd.core.helpers import ConnectHelper

from set_rpm import (DEFAULT_ELF, MAX_RPM, MIN_RPM, SYMBOL, TARGET,
                     find_symbol)

PROBE = "ATML2419050200001722"   # the stimulator's EDBG - never the Atmel-ICE

# SAM C21 TCC0: PER is at +0x40. The crank pattern's slot period, in timer
# counts; it varies as 1/rpm, so doubling the speed should halve it.
TCC0_PER = 0x42002400 + 0x40


def points(low, high, step):
    up = list(range(low, high + 1, step))
    if up[-1] != high:
        up.append(high)
    return up + up[-2:0:-1]


def main():
    ap = argparse.ArgumentParser(
        description="Sweep the stimulator's RPM over one debug session, "
                    "checking the crank timer follows.")
    ap.add_argument("--low", type=int, default=900)
    ap.add_argument("--high", type=int, default=3000)
    ap.add_argument("--step", type=int, default=150)
    ap.add_argument("--dwell", type=float, default=1.0,
                    help="seconds at each speed (default 1)")
    ap.add_argument("--cycles", type=int, default=0,
                    help="up-and-back sweeps to run; 0 runs until Ctrl+C")
    ap.add_argument("--hold", type=int,
                    help="set this one speed, check the timer followed, exit")
    ap.add_argument("--quick", action="store_true",
                    help="write only, check the timer at the ends, print every "
                         "100 rpm - for fine steps, where checking each one "
                         "would take longer than the step itself")
    ap.add_argument("--elf", default=str(DEFAULT_ELF))
    args = ap.parse_args()

    for v in (args.low, args.high, args.hold or MIN_RPM):
        if not MIN_RPM <= v <= MAX_RPM:
            sys.exit("%d rpm is outside %d..%d" % (v, MIN_RPM, MAX_RPM))

    addr, _size = find_symbol(args.elf, SYMBOL)

    with ConnectHelper.session_with_chosen_probe(
            unique_id=PROBE, target_override=TARGET,
            options={"connect_mode": "attach"}) as session:
        target = session.board.target

        def set_and_check(rpm, previous):
            target.write16(addr, rpm)
            time.sleep(0.05)            # a few overflows at any speed
            readback = target.read16(addr)
            per = target.read32(TCC0_PER)
            note = ""
            if readback != rpm:
                note = "  WRITE DID NOT STICK"
            elif previous and previous[0] != rpm and per == previous[1]:
                note = "  TIMER DID NOT MOVE - the crank speed is not changing"
            elif previous and previous[0] != rpm:
                expected = previous[1] * previous[0] / rpm
                if abs(per - expected) > 0.05 * expected:
                    note = "  timer moved, but not by 1/rpm (expected ~%d)" % expected
            print("%5d rpm   VRG_Rpm %5d   TCC0.PER %7d%s"
                  % (rpm, readback, per, note), flush=True)
            return (rpm, per)

        print("attached to %s, %s at 0x%08x, TCC0.PER at 0x%08x"
              % (PROBE, SYMBOL, addr, TCC0_PER))
        state = (target.read16(addr), target.read32(TCC0_PER))
        print("now: %d rpm, TCC0.PER %d" % state)

        if args.hold:
            set_and_check(args.hold, state)
            return

        path = points(args.low, args.high, args.step)
        cycle = 0
        try:
            while args.cycles == 0 or cycle < args.cycles:
                for i, rpm in enumerate(path):
                    ends = i == 0 or i == len(path) - 1 or rpm == args.high
                    if args.quick and not ends:
                        # flush() matters: pyOCD queues writes into CMSIS-DAP
                        # packets and sends them when the packet fills. Without
                        # it ~50 steps landed at once - a 10 rpm sweep reached
                        # the crank as 500 rpm jumps.
                        target.write16(addr, rpm)
                        target.flush()
                        if rpm % 100 == 0:
                            print("%5d rpm" % rpm, flush=True)
                        state = (rpm, state[1])
                    else:
                        # In quick mode the period in hand is from the last
                        # CHECKED step, not the last written one, so comparing
                        # against it would cry wolf - just report it.
                        state = set_and_check(rpm, None if args.quick else state)
                    time.sleep(args.dwell)
                cycle += 1
            state = set_and_check(args.low, state)
        except KeyboardInterrupt:
            print("\nstopped at %d rpm" % state[0])


if __name__ == "__main__":
    main()
