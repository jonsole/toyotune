# toyotune

Reverse engineering and ROM tuning for Toyota ECUs built on the Toshiba/Denso
8X (D8X) microcontroller — a proprietary part with an instruction set derived
from the Motorola 68HC11.

The best entry point is
**[Inside the 3S-GTE ECU](roms/3S-GTE/gen3/ecu_overview.md)**, an
architectural tour of the 1993 SW20 MR2 turbo ECU: three processors, adaptive
fuel trim and per-cylinder knock learning in 16 KB of ROM.

## What's here

- **[`roms/`](roms/)** — disassembly, annotation and tuned-ROM production.
  One directory per engine family (1G-GTE, 1G-GZE, 1JZ-GTE, 1UZ-FE, 3S-GE,
  3S-GTE, 3VZ-FE, 4A-GE), each holding one subdirectory per ECU part number.
  - [`roms/3S-GTE/`](roms/3S-GTE/) is the most thoroughly documented family —
    its [README](roms/3S-GTE/README.md) maps every part number to market,
    vehicle and CPU, and [`gen3/`](roms/3S-GTE/gen3/) holds the subsystem
    write-ups.
  - [`roms/docs/`](roms/docs/) documents the D8X MCU itself — instruction
    set, opcode matrix, registers — which every family here shares.
  - [`roms/d8x_assembler/`](roms/d8x_assembler/) is a Python reimplementation
    of the D8X assembler, and what the Makefiles actually build with.
- **[`hw/`](hw/)** — the Toyotune interface board: PCB, CPLD and host-MCU
  firmware for reading and rewriting these ECUs, including live access while
  the engine is running.
- **`sw/`** — IDA Pro installs used to open the `.idb` databases.

## Status

Ongoing and incomplete. The 3S-GTE Gen 3 pair is the furthest along; other
families are largely raw disassembly. Coverage is stated honestly in each
document — see
[session_journal.md](roms/3S-GTE/gen3/session_journal.md) for what is done
and what is pending.
