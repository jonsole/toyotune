# 3S-GTE ECUs

Toyota's 2.0 L turbocharged four, used in the SW20 MR2 and the ST205 Celica
GT-Four. Every ECU here is built on the Toshiba/Denso 8X (D8X) MCU.

**Start with [gen3/ecu_overview.md](gen3/ecu_overview.md)** — an architectural
tour of the Gen 3 ECU: the three-processor layout, the inter-CPU DMA link,
ignition and injection scheduling, the fuel trims, and knock learning. It is
the best entry point to everything else in this directory.

---

## One CPU or two, depending on the application

How many D8X CPUs an ECU carries is **not a generational progression** — it
tracks what the application needs:

| Application | CPUs |
|---|---|
| Naturally aspirated, manual transmission | **one** |
| Automatic transmission | **two** |
| Turbo (all the ECUs in this directory) | **two** |

So a second CPU is a standard option across Denso's range of the period,
fitted where the work justifies it, rather than something special to this
engine. Every ECU documented here is turbocharged, so every one of them is a
two-CPU design.

Each CPU has its own 16 KB ROM, and the pair exchange a 38-byte frame every
4 ms over a serial DMA link. Both halves are needed to understand — or to
modify — one ECU.

The two part numbers differ by **+10 in the last two digits**: `-9651`/`-9661`,
`-0461`/`-0471`, `-0481`/`-0491`. The lower number is CPU1 (real-time I/O:
ignition, injection, ADC, idle, knock, lambda); the higher is CPU2
(arithmetic: VE maps, load calculation, boost, diagnostics).

---

## ROM map

| ROM | Market | Vehicle | Toyota no. | Role | In repo |
|---|---|---|---|---|---|
| `D151802-4840` | USDM | SW20 MR2, Gen 1 | `89661-` | — | not present |
| `D151802-9361` | JDM | SW20 MR2, Gen 2 | `89861-17360` | — | bin, idb |
| `D151803-9651` | JDM | SW20 MR2, **Gen 3** | `89861-17460` | **CPU1** | ASM, bin, idb, XDF, `Claude/` |
| `D151803-9661` | JDM | SW20 MR2, **Gen 3** | `89861-17460` | **CPU2** | ASM, bin, idb, XDF, `Claude/` |
| `D151804-0461` | JDM | ST205 Celica GT-Four | | **CPU1** | ASM, bin, idb, XDF, `Claude/` |
| `D151804-0471` | JDM | ST205 Celica GT-Four | | **CPU2** | ASM, bin, idb, XDF |
| `D151804-0481` | UK | ST205 Celica GT-Four | | **CPU1** | ASM, bin, idb |
| `D151804-0491` | UK | ST205 Celica GT-Four | | **CPU2** | idb only — no ROM image |
| `D151804-7720` | JDM | ST205 Celica GT-Four, 95+ | | unpaired here | bin, idb — no disassembly |

Notes on the gaps:

- **`D151802-4840`** (USDM Gen 1) is listed in `roms.txt` but no directory
  exists for it — the image is not in this repo.
- **`D151804-0491`** has an IDA database but no `.bin`. Its CPU1 partner
  `-0481` is complete, so the UK ST205 pair cannot currently be built or
  cross-referenced in full.
- **Gen 1 and Gen 2** appear as single part numbers. Since both are turbo,
  the pattern above suggests a partner ROM exists and is simply absent from
  this collection rather than those ECUs being single-CPU — but that has not
  been confirmed for these two specifically.
- **`D151804-7720`** has no partner listed in `roms.txt`, so which CPU it is
  has not been confirmed. It has no disassembly either — only the ROM image
  and an IDA database.
- Only `-9651`, `-9661` and `-0461` have `Claude/` working copies, which are
  ahead of the parent `.ASM` in renames and comments. Edit those, not the
  parent, when doing RE work.

---

## Non-stock directories

| Directory | What it is |
|---|---|
| `HiTech-ROM Amuse` | Modified Gen 3 pair — stock, modified, 16K and descrambled variants of `-9651`/`-9661` |
| `Jon_ST205_ECU` | Personal tune branch off the JDM ST205 pair (`-0461`/`-0471`), with its own `Makefile` |
| `Unknown Techtom MR2` | A Techtom-tool ROM based on `D151803-9651`, origin unknown |

Also here: `3S-GTE_maps.xlsx`, `3S-GTE_IGN_ST205_map_diff.xlsx` and
`convert.xlsx` (extracted and diffed calibration maps).

---

## Documentation

**[`gen3/`](gen3/)** — reverse-engineering write-ups for the Gen 3 SW20 pair
(`D151803-9651` / `-9661`), the most heavily annotated ECU in the repo and the
reference implementation when a similar routine turns up elsewhere.

| Document | Covers |
|---|---|
| [ecu_overview.md](gen3/ecu_overview.md) | **Start here** — architecture, the CPU split, and how the subsystems fit together |
| [fuel_calculation_system.md](gen3/fuel_calculation_system.md) | Injector pulse-width chain, short/long-term trims, DMA load terms |
| [ignition_system.md](gen3/ignition_system.md) | CPR scheduling, dwell, advance blending, misfire detection |
| [knock_sensor_system.md](gen3/knock_sensor_system.md) | Knock MCU protocol, per-cylinder retard learning |
| [idle_control_system.md](gen3/idle_control_system.md) | ISCV target calculation, fixed-opening override, idle trim |
| [adc_system.md](gen3/adc_system.md) | Channel map, scan phases, sensor scaling, counter tick rates |
| [knock_mcu_update.ASM](gen3/knock_mcu_update.ASM) | A fully-annotated routine kept as the annotation-style reference |
| [session_journal.md](gen3/session_journal.md) | Progress log and pending work — read before starting new RE |

The **MCU itself** is documented at [`../docs/`](../docs/), outside this
directory, because every engine family in the repo uses the same D8X core:
[toshiba-8x-technical-reference.md](../docs/toshiba-8x-technical-reference.md)
(instruction set, opcode matrix, registers) plus its
[part1](../docs/toshiba-8x-reference-part1.md) and
[appendix](../docs/toshiba-8x-reference-part2-appendix.md).

---

## Building

From any ECU directory:

```
make.exe rom             # assemble + checksum -> output/<name>.bin
make.exe rom_toyotune    # + pad to 32K for the Toyotune flasher
make.exe rom_techtom     # + scramble/XOR-encode for the Techtom tool
make.exe clean
```

From this directory, the same targets fan out across every ECU in the `ROMS`
list in `Makefile`. Shared rules live in `makefile.lib`. Use `roms/bin/make.exe`
rather than a generic `make` — the rules rely on Windows `mkdir`/`rmdir`
behaviour.
