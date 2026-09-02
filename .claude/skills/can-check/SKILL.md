---
name: can-check
description: Read the Toyotune board's CAN telemetry and decide whether the bus is working. Use when asked to check the CAN bus, read telemetry, confirm the ECU is reporting, or diagnose missing/stopped CAN traffic on the bench rig.
---

# Checking the Toyotune CAN bus

The board sniffs the ECU's inter-CPU DMA link and republishes selected fields
as periodic CAN frames. This skill reads those frames and, when they are
missing, works out why.

## 1. Read the bus

```
cd hw/toyotune_lv_2p1/sw/python
.venv/Scripts/python.exe -u can_monitor.py
```

Add `--all` to show frames whose identifiers are not in `toyotune.dbc`, and
`--cpu2` for a CPU2 board (`0x420`+ instead of `0x400`+).

## 2. What healthy looks like

Five tiers, decoded from `toyotune.dbc`:

| id | tier | period |
|---|---|---|
| 0x400 | Fast | 20 ms |
| 0x401 | Medium1 | 100 ms |
| 0x402 | Medium2 | 100 ms |
| 0x403 | Slow | 500 ms |
| 0x404 | Raw | 50 ms |

Count frames over a fixed window and check the **ratios**, which prove the
scheduler is keeping time: Fast:Medium should be 5:1, Slow about 1:5 against
Medium. Identifiers are **11-bit standard** - a monitor written for the old
29-bit extended `0x1001`-`0x1003` scheme sees nothing.

Sanity-check the values too: `Rpm` should match whatever the stimulator is set
to (see `set_rpm.py`), and `Battery` should read around 12 V. `Tps` reading 0
and `Pim2` sitting at a constant is normal unless those inputs are wired.

Known outstanding: `0x404` Raw has been seen not transmitting while the other
four are fine. Do not treat its absence as a new fault without checking.

## 3. If there is no traffic

Do not guess. Read the CAN controller's own registers over SWD - the answer is
almost always in them. Peripheral addresses are fixed:

```
cd hw/toyotune_lv_2p1/sw/python
./.venv/Scripts/pyocd.exe cmd -t atsamc21j18a -u J41800034284 \
    -O connect_mode=attach \
    -c "read32 0x42001C40 8" -c "read32 0x42001C18 4" -c "read32 0x42001CC4 4"
```

| address | register | fields |
|---|---|---|
| 0x42001C40 | ECR | TEC bits 7:0, REC 14:8, CEL 23:16 (CEL clears on read) |
| 0x42001C44 | PSR | LEC 2:0, ACT 4:3, EP bit 5, EW bit 6, **BO bit 7** |
| 0x42001C18 | CCCR | INIT bit 0 - hardware sets it on bus-off |
| 0x42001CC4 | TXFQS | free level 5:0, put index 20:16, **TFQF bit 21** |

`-u J41800034284` is the Atmel-ICE (the Toyotune board). The Xplained Pro's
EDBG is `ATML2419050200001722` and is the **stimulator** - never flash or write
one thinking it is the other. `connect_mode=attach` is required: the default
halts the core, which stops the crank signals and makes the ECU lose sync.

### Decision table

| symptom | meaning | do |
|---|---|---|
| `TEC` climbing to 248, `BO`=1, `LEC`=5 (Bit0Error) | Transmitting but the bus never goes dominant - transceiver or wiring fault | Check transceiver VCC and VIO **at the chip**, then CANH/CANL |
| `TEC` parked at 128, `EP`=1, `LEC`=3 (AckError) | Benign. Nothing is acknowledging | Not a fault. Open a monitor and it recovers by itself |
| `TEC`=0, `TXFQS.TFQF`=1 (queue full), `ACT`=0 (Synchronizing) | Frames queued but the node is not allowed to start - RX looks permanently dominant | Check **VIO first** - RXD's output stage is powered from it, so an unpowered VIO pins RXD low |
| No frames queued at all, queue empty | Nothing is calling the transmit path | Check the DMA capture and the tick - see below |

**`LEC` is the field that separates a real fault from a quiet bus.** 3 means
nobody is listening; 5 means something is electrically wrong.

### If nothing is being queued

Work up the chain, all readable over SWD (derive addresses from the ELF, see
below):

- `ECU_Dma` - read it twice. Changing bytes mean the Denso is running and the
  SDL sniffer is capturing. Both direction-valid flags must be set: the
  telemetry task sends nothing until **both** are, which the older three-frame
  firmware did not require.
- `CanTelemetry_TickMs` - read twice. Not advancing means SysTick is not
  running, so the telemetry task is never signalled.

## 4. Firmware counters

```
arm-none-eabi-nm build/mr2-cpu1-release/toyotune_denso.elf | grep -E "CAN_TxDropped|CAN_BusOffRecoveries"
```

Derive addresses this way every time - they live in `.bss` and move whenever a
rebuild shifts the layout.

- **`CAN_TxDropped`** - frames discarded because the Tx queue stayed full past
  the bounded wait. **Cumulative since reset, so the absolute value means
  little.** Compare two readings a few seconds apart; rising *while a monitor
  is attached* is a real problem, rising when the bus is unattended is normal.
- **`CAN_BusOffRecoveries`** - times the controller has been brought back from
  bus-off. Non-zero means it got there at least once.

## 5. Gotchas that have wasted time before

- **`kill -9` on the monitor does not stop the CANable.** Its CAN controller
  stays started in hardware and keeps ACKing on its own, so the bus is not
  actually silent. Call `bus.shutdown()` to genuinely silence it.
- **A lone node cannot reach bus-off from missing ACKs.** ISO 11898-1 does not
  increment TEC for an error-passive transmitter that sees no dominant bit
  while sending a passive error flag, so it parks at TEC 128. Bus-off means
  something electrical, not an absent listener.
- **Check USB enumeration before believing the board is dead.** The CANable,
  Atmel-ICE and EDBG have each dropped off USB independently on this bench, and
  it looks exactly like a board fault.
