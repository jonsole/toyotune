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
tracks how much work the application asks for:

| Application | CPUs |
|---|---|
| Naturally aspirated, manual transmission | **one** |
| Automatic transmission | **two** |
| Turbo (every ECU in this directory) | **two** |

The sharpest comparison is within Gen 3 itself: the naturally-aspirated
Gen 3 ECU does **everything in a single CPU**, including the same class of
fuelling calculation. So it is not speed density on its own that needs a
second processor — it is the extra load the turbo application piles on top.
Larger maps, boost control, and the additional corrections that come with
them are the plausible drivers, though the precise trigger has not been
established.

A second CPU is therefore a standard option across Denso's range of the
period, fitted where the work justifies it, rather than something special to
this engine.

Each CPU has its own 16 KB ROM, and the pair exchange a 38-byte frame every
4 ms over a serial DMA link. Both halves are needed to understand — or to
modify — one ECU.

The two part numbers differ by **+10 in the last two digits**: `-9651`/`-9661`,
`-0461`/`-0471`, `-0481`/`-0491`. The lower number is CPU1 (real-time I/O:
ignition, injection, ADC, idle, knock, lambda); the higher is CPU2
(arithmetic: VE maps, load calculation, boost, diagnostics).

---

## Two different fuelling architectures

The ECUs here do not all measure load the same way, and this is the single
most important thing to know before assuming one generation's documentation
applies to another:

| | Load measurement |
|---|---|
| SW20 MR2, **Gen 1 and Gen 2** | **Air flow meter** — airflow is measured directly |
| SW20 MR2 **Gen 3**, and the **ST205** | **Speed density** — airflow is inferred from manifold pressure, RPM and a volumetric-efficiency map |

Everything in [`gen3/`](gen3/) describes a speed-density ECU. Its fuelling,
its load term, and the DMA traffic that carries them do not transfer to Gen 1
or Gen 2.

---

## ROM map

| ROM | Market | Vehicle | Toyota no. | Role | In repo |
|---|---|---|---|---|---|
| `D151802-4840` | USDM | SW20 MR2, Gen 1 | `89661-` | — | not present |
| `D151802-9361` | JDM | SW20 MR2, Gen 2 | `89861-17360` | — | bin, idb |
| `D151803-9651` | JDM | SW20 MR2, **Gen 3** | `89861-17460` | **CPU1** | ASM, bin, idb, XDF, `Claude/` |
| `D151803-9661` | JDM | SW20 MR2, **Gen 3** | `89861-17460` | **CPU2** | ASM, bin, idb, XDF, `Claude/` |
| `D151804-0461` | JDM | ST205 Celica GT-Four | | **CPU1** | ASM, bin, idb, XDF, `Claude/` |
| `D151804-0471` | JDM | ST205 Celica GT-Four | | **CPU2** | ASM, bin, idb, XDF, `Claude/` |
| `D151804-0481` | UK | ST205 Celica GT-Four | | **CPU1** | ASM, bin, idb, `Claude/` |
| `D151804-0491` | UK | ST205 Celica GT-Four | | **CPU2** | idb only — no ROM image |
| `D151804-7720` | JDM | ST205 Celica GT-Four, 95+ | | **CPU1** — revision of `-0461`, runs against `-0471` | bin, idb — no disassembly |

Notes on the gaps:

- **`D151802-4840`** (USDM Gen 1) is listed in `roms.txt` but no directory
  exists for it — the image is not in this repo.
- **`D151804-0491`** has an IDA database but no `.bin`. Its CPU1 partner
  `-0481` is complete, so the UK ST205 pair cannot currently be built or
  cross-referenced in full.
- **Gen 1 and Gen 2** appear as single part numbers, and it is genuinely
  open whether that reflects a missing partner or a single-CPU design. They
  measure airflow directly (see below), so the speed-density arithmetic that
  occupies CPU2 in the Gen 3 ECU is not work they have to do — the second CPU
  may simply not be warranted. Not established either way.
- **`D151804-7720`** has no partner listed in `roms.txt`, and it does not need
  one: it is a later **CPU1** that runs against the existing `-0471`, and no
  revised CPU2 was ever made. It is `-0461` with 21 bytes of code inserted plus
  a calibration refresh, established by diffing the two images:
  - The 16400-byte `.BIN` is 16384 bytes of ROM based at `C000` followed by a
    repeat of its own last 16 bytes — a dump artifact, not a header. Compare
    from offset 0 or every address comes out 16 bytes wrong.
  - It tracks `-0461` at shift 0 from `C000` to `D8FD` (identical for the first
    1627 bytes, then ~25 scattered single-byte calibration differences).
  - At `D8FD` a 65-byte block becomes an 86-byte one: **+0x15 bytes**, in two
    pieces — 11 bytes after the first test, ~10 more shortly after — with the
    preceding branch displacements grown by `0x0B` to match.
  - From `D93E` (`-0461`) / `D953` (`-7720`) upward it is `-0461` shifted by
    `+0x15`, with 102 small clusters (199 bytes) of further calibration change.
  - The vector table is patched to suit: `F66C`→`F681`, `F714`→`F729`,
    `EF26`→`EF3B`. `C003` and `C5E2` are unchanged, their targets being below
    the insert.

  `D8FD` lands in the **open-loop vs closed-loop selection for base injector
  pulse width** — the gating chain from `loc_D8F6` to `loc_D922` that picks
  between `init_pw_closed_loop` and `init_pw_open_loop` — so the insert reads
  as extra conditions on closed-loop entry. Every input that chain tests
  (`dmarx_enrichment_unk_22B`, `dmarx_warmup_enrichment_22A`,
  `dmarx_enrichment_unk_230`, `dmarx_status1_23C`, `var_io_input1`,
  `var_flags_40`/`_46`) already arrives in the existing 34-byte frame, which is
  consistent with the DMA layout never having had to change. The inserted
  opcodes have not been disassembled — that is the open question here, not what
  the ROM is. Since `-0461` has an annotated `Claude/` copy and the two are
  near-identical, `roms/rom_port/` should name `-7720` cheaply.
- Only `-9651`, `-9661`, `-0461`, `-0471` and `-0481` have `Claude/` working copies, which are
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
