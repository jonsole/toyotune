---
name: stimulator
description: Drive the bench engine stimulator - set or sweep RPM, adjust knock severity, build and flash it. Use when asked to change engine speed, simulate knock, make the ECU think an engine is running, or work on the stimulator firmware.
---

# Controlling the engine stimulator

The stimulator runs on a **SAM C21 Xplained Pro** and generates the signals the
ECU expects from a running engine, so the ECU can be exercised on the bench.
Source is `hw/toyotune_lv_2p1/sw/samc2x/stimulator`.

Its debug probe is the EDBG, unique id **`ATML2419050200001722`**. The other
probe on this bench, `J41800034284`, is the Atmel-ICE on the **Toyotune board**.
Never act on one thinking it is the other.

## What it generates

Crank and cam (NE, G1, G2) from TCC0's pattern generator, a TDC marker, a knock
burst on the DAC, and throttle and MAP on an SPI DAC. `vrg.c` and `knock.c`
carry the full pin maps, Xplained Pro header positions and the conditioning
network - read those rather than re-deriving.

Currently wired to the ECU: **NE, G1, G2**, and the ECU's **IGT** back into
PB08. Throttle (`PB05`) and MAP (`PA27`) are generated but **not connected**,
which is why `Tps` reads 0 in telemetry.

## Setting RPM

This is the main control. `VRG_Rpm` is re-read by `TCC0_Handler` on every
overflow, so writing it over SWD changes crank speed within one pattern slot.

```
cd hw/toyotune_lv_2p1/sw/python
.venv/Scripts/python.exe set_rpm.py --probe ATML2419050200001722            # read
.venv/Scripts/python.exe set_rpm.py --probe ATML2419050200001722 2500       # set
.venv/Scripts/python.exe set_rpm.py --probe ATML2419050200001722 \
        --sweep 1000 6000 --step 500 --dwell 3                              # sweep
```

`--probe` is only needed while both probes are attached, but the script refuses
to guess rather than risk writing into the wrong target.

**For sweeps, use `sweep_rpm.py`, not `set_rpm.py --sweep`.** It attaches once
and stays, where each `set_rpm.py` run re-attaches (a couple of seconds each),
and it reads back `TCC0.PER` - the crank timer's actual period - so a step that
did not reach the hardware says so rather than only proving a word of RAM
changed:

```
.venv/Scripts/python.exe sweep_rpm.py                          # 900..3000..900, forever
.venv/Scripts/python.exe sweep_rpm.py --cycles 1 --step 10 --dwell 0.05 --quick
.venv/Scripts/python.exe sweep_rpm.py --hold 2500              # one value, checked
```

`--quick` skips the per-step check for fine steps and **must flush after every
write** - it does. pyOCD queues writes into CMSIS-DAP packets and only sends
them when a packet fills or something reads back, so without `target.flush()`
about 50 writes land at once: a 10 rpm sweep reached the crank as 500 rpm
jumps on 2026-09-29, and it looked exactly like the ECU or the dash quantising
rpm. The endpoints checked correct throughout, because a readback forces the
flush.

- Range is **50 to 10000**. **Never write 0** - `VRG_CalcPerBufValue()` divides
  by it and the Cortex-M0+ has no hardware divider, so it yields a nonsense
  period rather than faulting.
- Verify the effect in CAN telemetry: `Rpm` in the `0x400` Fast frame.
- The ECU reads back **0.2 to 0.6% low**, widening slightly with RPM. That is
  the ECU's own resolution - it derives RPM from `ne_sum3`, the time for three
  NE pulses in 4 us units - not an error in the stimulator.

## Knock

`Knock_Severity` is a plain global, so it is tunable live the same way:

```
./.venv/Scripts/pyocd.exe cmd -t atsamc21j18a -u ATML2419050200001722 \
    -O connect_mode=attach -c "write8 <addr> 0x40"
```

The burst is 6.7 kHz, 64 samples at 53.6 kHz - 1.19 ms, eight cycles, decaying
geometrically to mid-scale. Peak deviation from mid-scale is
`(Severity / 256)` of half the DAC range; with the board on 5 V, mid-scale is
2.5 V, so severity 64 gives roughly +/-0.55 V.

Three things gate whether the ECU reacts at all:

- **It fires from the ECU's own IGT**, via `TCC1_Handler` in `igt.c` on the
  falling edge at PB08. No IGT wired means no bursts. `IGT_CaptureTime` staying
  zero confirms that case.
- **The ECU only looks for knock between 700 and 7200 RPM.** Outside that
  window the signal is ignored.
- **Diagnostic mode does NOT stop the ECU computing retard.** With TE1-E1
  jumpered the ECU forces fixed timing (`ignition_system.md`: "-10 deg BTDC,
  Test mode"), and this note used to conclude that knock retard therefore had
  nothing to act on. Wrong: on 2026-09-29, jumper in, the ECU pulled 13 counts
  (6.5 deg) at 2400 rpm and reported it in telemetry. Whether that retard
  reaches the actual spark in test mode is unverified - but the value is live,
  and anything reading it (the dash's knock warning) will react.

**At the default severity the bench knocks permanently.** `Knock_Severity`
boots at 64 - a hard knock on every ignition event. Retard decays 2 counts per
4 ms between events, so below about 2400 rpm the decay wins and retard stays
near zero; above it the bursts outpace the decay and it climbs to around
6.5 deg, dropping back out at about the same speed on the way down. That was
enough to trip the dash's 3 deg knock warning continuously. Write 0 to stop the
ECU retarding at all, or 8-16 for occasional, more realistic knock.

`PB09` also wants the `PA20` TDC marker looped back for `IGT_TimingPeriod` to
work - both are on EXT1, so it is a short jumper.

## Building and flashing

```
cd hw/toyotune_lv_2p1/sw/samc2x/stimulator
cmake --preset release && cmake --build --preset release      # or: debug
../../python/.venv/Scripts/pyocd.exe flash -t atsamc21j18a \
        -u ATML2419050200001722 build/release/stimulator.hex
../../python/.venv/Scripts/pyocd.exe reset -t atsamc21j18a -u ATML2419050200001722
```

**Always pass `-u`.** With one probe attached pyOCD selects it silently, so a
missing `-u` can flash the stimulator image onto the Toyotune board - both are
ATSAMC21J18A and nothing will object.

## Deriving symbol addresses

Always read them from the ELF; they live in `.bss`/`.data` and move whenever a
rebuild shifts the layout. `VRG_Rpm` moved from `0x2000161c` to `0x2000163c`
inside a single session.

```
arm-none-eabi-nm build/release/stimulator.elf | grep -E "VRG_Rpm|Knock_Severity|IGT_"
```

`set_rpm.py` already does this for itself and needs no address.

## Gotchas

- **`connect_mode=attach` is mandatory.** pyOCD's default halts the core, which
  stops the crank signals dead and makes the ECU lose sync.
- **Leave the stimulator's CAN off the bus.** `main()` still calls
  `CAN_Tx(CAN_TestTxId++, "Hello", 5)` unthrottled on an incrementing
  identifier, which would saturate the bus and collide with the Toyotune
  board's telemetry. Keep `PA24`/`PA25` disconnected.
- Because of that call the main loop wedges in `CAN_Tx`'s wait once its queue
  fills. Harmless for signal generation - VRG runs off timers and DMA, and
  `VRG_SetRpm` has already run - but the `IGT_GetTimingPeriod()` readback in
  that loop stops.
- **Check USB enumeration before believing the board is dead.** The EDBG,
  Atmel-ICE and CANable have each dropped off USB independently on this bench.
