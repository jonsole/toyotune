---
name: canable
description: Drive the CANable Pro adapter directly - open the bus, send and receive arbitrary CAN frames, log traffic, and read or write the running ECU's memory over the diagnostic frames. Use when asked to send a CAN frame, sniff or log the bus, talk to a specific identifier, or do a live ECU memory read/write over CAN.
---

# Driving the CANable Pro

The bench adapter is an **MKS CANable Pro V1.0** on the `gs_usb` interface,
running **classic CAN at 500 kbit/s with 11-bit standard identifiers**.

This skill is for *driving* the adapter - sending frames, sniffing, logging,
talking to a particular identifier. For "is the bus working and why not", use
**`can-check`** instead: it goes to the CAN controller's registers over SWD,
which is a different job.

Everything below runs from `hw/toyotune_lv_2p1/sw/python` with that
directory's `.venv`, which already has `python-can`, `gs_usb`, `pyusb` and
`libusb-package`.

## 1. Opening the bus

Two lines that are not optional and are easy to omit:

```python
import os, pathlib
import libusb_package
os.environ["PATH"] = (str(pathlib.Path(libusb_package.__file__).parent)
                      + os.pathsep + os.environ["PATH"])
import can

bus = can.Bus(interface="gs_usb", channel=0, index=0, bitrate=500000)
```

`gs_usb` calls `usb.backend.libusb1.get_backend()` with no arguments, which
finds `libusb-1.0.dll` only if it is on PATH. The copy inside `libusb-package`
is not, so it has to be put there **before `can` is imported**.

**Never open listen-only.** Two independent reasons:

- ACK is a transmit action. With one Toyotune board plus this adapter, the
  adapter is the only other station; silent, it leaves the board
  error-passive with nothing acknowledging its frames.
- `CAN_TxStandard()` in `can.c` waits on a bounded spin for a queue slot. An
  adapter that does not ACK fills the 8-deep Tx queue and starts dropping
  telemetry.

**Always `bus.shutdown()`.** Killing the process does *not* stop the adapter:
its controller stays started in hardware and keeps ACKing on its own, so the
bus is not actually silent afterwards. Use `try/finally`.

## 2. Sniff, count, filter

Counting identifiers over a window is the fastest way to see what is alive:

```
cd hw/toyotune_lv_2p1/sw/python
.venv/Scripts/python.exe -c "
import os, pathlib, collections
import libusb_package
os.environ['PATH'] = str(pathlib.Path(libusb_package.__file__).parent) + os.pathsep + os.environ['PATH']
import can
bus = can.Bus(interface='gs_usb', channel=0, index=0, bitrate=500000)
try:
    seen = collections.Counter()
    for _ in range(500):
        m = bus.recv(timeout=1.0)
        if m is None: break
        seen[m.arbitration_id] += 1
    for i in sorted(seen): print('  0x%03X  %d' % (i, seen[i]))
finally:
    bus.shutdown()
"
```

Healthy, with the stimulator running, is seven identifiers in roughly these
proportions over a few seconds - the ratios prove the sender's scheduler is
keeping time:

| id | frame | period | relative rate |
|---|---|---|---|
| 0x400 | Fast | 20 ms | 5x Medium |
| 0x401/2/5 | Medium1/2/3 | 100 ms | baseline |
| 0x403 | Slow | 500 ms | 1/5 Medium |
| 0x404 | Raw | 50 ms | 2x Medium |
| 0x406 | Info | 1000 ms | 1/10 Medium |

To decode rather than count, use `can_monitor.py` - it reads the layouts from
`toyotune.dbc` so it cannot drift from the firmware.

Hardware filters cut host load when chasing one identifier:

```python
bus.set_filters([{"can_id": 0x40B, "can_mask": 0x7FF, "extended": False}])
```

## 3. Sending a frame

```python
msg = can.Message(arbitration_id=0x40A,
                  data=[0x01, 0x02, 0x00, 0, 0, 0, 0, 2],
                  is_extended_id=False)      # 11-bit - the default is 29-bit
bus.send(msg, timeout=1.0)
```

**`is_extended_id=False` matters.** All Toyotune telemetry and diagnostics use
11-bit standard identifiers. An extended frame with the same number is a
different frame and nothing will answer it.

Before sending anything, know what already owns that identifier. `0x400`-`0x406`
are CPU1 telemetry and `0x420`-`0x426` CPU2; transmitting on one of those
collides with the board, which cannot be resolved by arbitration and produces
bit errors on both sides.

## 4. Reading and writing ECU memory over CAN

This is the interesting capability. `diag_can.c` maps the Denso serial
diagnostic link onto two frames, so the running ECU's memory is reachable from
the host with no debugger attached.

- **Command in:** `0x40A` (CPU1) / `0x42A` (CPU2)
- **Response out:** `0x40B` / `0x42B`

Command frame, 8 bytes, big-endian to match the ECU:

| byte | meaning |
|---|---|
| 0 | opcode |
| 1-2 | address |
| 3-4 | value (writes only) |
| 5-6 | period ms (add-periodic only) |
| 7 | size, 1 or 2 - **honoured for writes, ignored for reads**, see below |

Response frame:

| byte | meaning |
|---|---|
| 0 | opcode echoed |
| 1 | status, 0 = ok |
| 2-3 | address echoed - **match on this**, replies are not ordered |
| 4-5 | value |
| 6-7 | zero |

| opcode | | status | |
|---|---|---|---|
| 0x01 | read | 0x00 | ok |
| 0x02 | write | 0x01 | bad opcode |
| 0x03 | add periodic | 0x02 | no space |
| 0x04 | cancel periodic | 0x03 | not found |
| 0x05 | cancel all | 0x04 | bad size |
| | | 0x05 | busy |

A periodic read emits the same response layout on its own schedule, so a host
decodes one thing for both. The pool is **8 entries**; `0x05` cancels the lot.

### Every read is 16 bits, whatever size you ask for

**Confirmed on the bench 2026-09-08.** The `size` byte is validated and
stored, and it does change a *write* - the ECU protocol has both `0xDC`
write-8 and `0xDD` write-16 - but the only read command it has is `0xDA`,
**read-16**. There is no 8-bit read. `DiagCan_ReadComplete()` then passes the
full 16-bit value into the response without masking it by the requested size,
so `size=1` and `size=2` return byte-for-byte identical replies.

The practical consequence: reading an 8-bit variable gives you **that byte in
the high half and its neighbour in the low half**.

```
read 0x020C size=1  ->  0x9B50
read 0x020C size=2  ->  0x9B50      (identical, 8 reads each)

0x9B = 155  Battery     -> 0.0774*155 + 0.0601 = 12.06 V
0x50 =  80  NvTrimPim   -> the next byte along
```

Both halves matched the telemetry frames exactly at the same moment, so this
is the ECU's real memory and not a decode artefact. **Take `value >> 8` when
you asked for a byte**, and be aware you have also read its neighbour - which
is harmless for a read, but means you cannot infer anything from the low byte
without knowing what lives there.

### Other things worth knowing before trusting a result

- **Match replies on the echoed address, not on arrival order.** Periodic
  reads interleave with one-shot ones.
- **Reads are not provably lossless.** A 163k-read soak against a constant ROM
  location saw about 1 wrong value in 27,000, and nothing in the protocol
  detects a bad read - only writes are verified, by the ECU reading back what
  it wrote. Read anything that matters twice.
- **"Read twice" only proves anything for a value that should be static.** A
  live variable legitimately differs between reads: two reads of `RpmX5p12`
  during a steady 3500 rpm came back 17880 and 17888, which is 3492 and 3494
  rpm - both correct, and both matching the telemetry frame at the time.
  Comparing repeated reads of a moving value tests nothing.
- **Every access is a single D8X instruction**, so engine code never sees a
  half-written value. A byte write is a real byte write, not a
  read-modify-write, so it cannot disturb its neighbour.
- **Bound every drain.** The ECU's block-read mode streams until a stop
  command lands, and that command is often missed - which is why
  `Diag_ReadStrays` is normally large and is not a fault.

Full protocol write-up: `roms/3S-GTE/gen3/diag_protocol.md`.

## 5. Logging

`python-can` writes several formats; `.blf` and `.asc` open in most CAN tools,
`.csv` is easiest to post-process.

```python
with can.Logger("capture.blf") as logger:
    notifier = can.Notifier(bus, [logger])
    ...
    notifier.stop()
```

Replay a capture back onto the bus with `can.LogReader` and `bus.send()` -
useful for exercising a dash node with recorded traffic when the ECU rig is
not powered.

## 6. Gotchas that have cost time

- **Only one process can hold the adapter.** `gs_usb` needs exclusive USB
  access, so a second reader fails with `OSError: access violation` inside
  `libusb_open` rather than anything that names the real problem. A stray
  script left running has caused exactly this. Check for other Python
  processes before assuming hardware.
- **`kill -9` leaves the adapter transmitting ACKs.** See §1.
- **Check USB enumeration before believing anything is dead.** The CANable,
  the Atmel-ICE and the EDBG have each dropped off USB independently on this
  bench, and it looks exactly like a board fault. `canable gs_usb` should be
  present with status OK.
- **A wedged adapter still enumerates.** If it is listed but every open fails,
  replug it; that has fixed it before.
- **The stimulator has its own CAN, and it must stay off the bus.** `main()`
  there still calls `CAN_Tx(CAN_TestTxId++, "Hello", 5)` unthrottled on an
  incrementing identifier, which would saturate the bus and collide with the
  telemetry. Keep its `PA24`/`PA25` disconnected.
- **Nothing on the ECU side moves unless the stimulator is running** - see the
  `stimulator` skill.
