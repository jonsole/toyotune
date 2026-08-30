# The 3S-GTE Gen 3 ECU — an overview

**Toyota part 89861-17460 · Denso `D151803-9651` + `D151803-9661` · JDM SW20 MR2, 1993–94**

This is the front door to the rest of `roms/docs/`. It describes what the ECU
is, how it is put together, and which parts are worth a second look. The
per-subsystem write-ups go deeper; this one is meant to be read start to
finish.

---

## The short version

It is a **three-processor** engine controller built from 8-bit Toshiba/Denso
8X parts, running speed-density fuelling with dual-rate adaptive fuel trim,
per-cylinder adaptive knock retard, and crank-angle-domain ignition
scheduling done in hardware compare registers.

Strip away the resolution and that is the same functional architecture a
modern ECU uses. What separates them is bits, cells and clock — not concept.

| | |
|---|---|
| Processors | 3 — CPU1, CPU2, and a dedicated knock MCU |
| ROM | 16 KB per CPU |
| Core | Toshiba/Denso 8X, "enhanced" variant (8 compare registers, serial DMA) |
| Timer | 19-bit, 4 µs per count |
| Crank resolution | 24 NE pulses/rev, 15° apart |
| Inter-CPU link | 1 MHz synchronous serial, DMA, 38-byte frame every 4 ms |
| Fuelling | Speed-density (VE × MAP × RPM), no MAF |

---

## Three processors, not two

Most 1993 ECUs were a single MCU. This one is three.

```
        ┌──────────────┐   knock level (3-bit, PORTB)   ┌───────────┐
        │  Knock MCU   │ ─────────────────────────────► │           │
        │  (D8X SDIP64)│ ◄───────────────────────────── │           │
        └──────────────┘   clock + TDC ref, DOUT.2 rst  │   CPU1    │
                                                        │  -9651    │
   piezo knock sensors ──►                              │           │
                                                        │ real-time │
                                                        └─────┬─────┘
                                                              │
                                     ASR2 / ASR3 serial DMA   │  38 bytes
                                     1 MHz, 4 ms frame        │  each way
                                                              │
                                                        ┌─────┴─────┐
                                                        │   CPU2    │
                                                        │  -9661    │
                                                        │           │
                                                        │   maps    │
                                                        └───────────┘
```

**CPU1 (`-9651`) owns everything with a deadline.** Ignition scheduling,
injector firing, the ADC scan, idle valve control, knock integration, and the
whole closed-loop lambda trim.

**CPU2 (`-9661`) owns the arithmetic.** The base VE maps, the speed-density
load term, boost control via the TVSV duty cycle, and the OBD datastream. It
does no lambda control of its own — it only receives CPU1's O2 reading to
pack a rich/lean bit into a diagnostic byte.

**The knock MCU** is a third D8X in an SDIP64 package wired to the piezo
sensors. It hands CPU1 a 3-bit knock level over `PORTB`, clocked against the
crank. CPU1 can hard-reset it by pulsing `DOUT.2` low for about 12 µs.

The split is the most consequential design decision in the box. One 8-bit
part could not do speed-density arithmetic *and* hold hard real-time
deadlines, so Toyota paid for a second MCU, a second ROM, and an inter-CPU
protocol that has to be kept in sync. That is an expensive answer to a
compute wall, and they paid it.

---

## The inter-CPU link

The two CPUs share one physical DMA buffer, exchanged as a 38-byte frame
every 4 ms over a 1 MHz synchronous serial link (`ASR2` receive, `ASR3`
transmit). Each CPU sees the same buffer at a **different base address**, and
the offset depends on direction:

| Direction | Formula |
|---|---|
| CPU2 → CPU1 (CPU2 `dmatx_*` = CPU1 `dmarx_*`) | `CPU1 = CPU2 + 0xDA` |
| CPU1 → CPU2 (CPU1 `dmatx_*` = CPU2 `dmarx_*`) | `CPU1 = CPU2 + 0x13B` |

Getting these the wrong way round is the single easiest mistake to make when
cross-referencing the two disassemblies: applying `0xDA` to a `dmatx` address
lands inside CPU2's serial receive buffer rather than its DMA block, and
produces a confident-looking answer about an unrelated variable.

Not every field in the frame is live. `dmarx_nv_trim_pim` — CPU1's learned
barometric trim — is written into CPU2's buffer every single frame and read
by absolutely nothing.

---

## Time and angle

Everything real-time hangs off a 19-bit hardware timer running at **4 µs per
count**, and the crank gives 24 NE pulses per revolution, one every 15°.

Ignition is computed in the NE interrupt as a *time offset* from the current
crank timestamp, then written into compare register `CPR0`. When the timer
matches, the hardware fires the interrupt that toggles the coil — the CPU is
not in the loop at the moment of the spark. `DOUT.0` low is charging, high is
firing; dwell is simply the interval between two `CPR0` events.

Because 15° of crank is coarse for spark timing, advance is interpolated
*between* teeth: the code measures `ne_sum3`, the time for the last 45° (3
pulses), and converts degrees to timer counts as `ne_sum3 / 45`, carrying a
fractional term. Effective resolution is about **0.5° per count**. Spark
confirmation comes back from the igniter as IGF on `ASR0`; missed events are
counted and can trigger fuel cut.

The ADC is external, reached over the serial port as a multiplexer: CPU1
sends a channel select and receives the result of the *previously* requested
channel, so every reading is one cycle stale by construction. Channels are
scanned on a priority schedule — TPS every 8 cycles, intake air temperature
every 32.

---

## Fuelling

CPU2 computes the load term — the base VE map indexed by RPM and MAP,
multiplied by RPM and by MAP again, i.e. the classic **VE × MAP × RPM**
speed-density airflow estimate — and ships it to CPU1 over the DMA buffer.

CPU1 turns that into an injector pulse width through roughly seven stages:
base pulse width from the load term, then the fuel-trim multiplier, an
ignition-blend term, per-cylinder trim tables, a limp-home fixed-width
override, an optional halving for full-cycle values, and finally an additive
battery-voltage dead-time compensation applied per firing.

Two details worth knowing about that chain: anything below `0x0D` (52 µs) is
rejected as too short to bother firing, and if the injector is already open
the code *extends* the current pulse rather than restarting it.

The asymmetry that trips people up is that the trims are multiplicative and
applied once centrally, the per-cylinder correction is a separate
multiplicative stage with its own tables, and the battery compensation is
additive and per-event. "Adding fuel" means three different operations
depending on where you do it.

---

## Three fuel trims

This is where the ECU is better than its era suggests.

**Short-term (STFT)** is `var_lambda_integrator` — plain RAM, never
persisted, reset to neutral on restart. It is driven by a textbook
jump-and-ramp controller: when the averaged O2 reading crosses rich (`≥0xB3`)
or lean (`≤0x4D`) the integrator takes a proportional *jump* of `0x07AE`;
otherwise it *ramps* by `0x0010` per tick in the direction of the sensor.
Between `0x4E` and `0xB2` is a deadband that exits without acting — which is
what stops the loop chattering around stoichiometric.

**Long-term (LTFT)** is a 12-byte table in battery-backed NV RAM, indexed by
manifold pressure with interpolation between cells, so the correction is
learned per load site. Closed throttle reads the idle cell directly; if the
trims are marked invalid it returns a neutral `0x80`.

The two are summed into a single multiplier — unity plus LTFT plus STFT —
and applied once.

**And then a third one**, which is not in that multiplier at all. A slow
global correction learned only under tightly-gated cruise: coolant between
82.9 and 103.8 °C, off idle, under 3200 rpm, battery at least 11.4 V, no
transient in progress, and CPU2 not asking for enrichment. Inside that
window it takes 17 O2 samples, **discards the first seven as settling**,
votes ±1 per sample, and only if the majority clears a deadband does it move
the stored value by a single step. It is kept as a value/complement pair for
integrity — and CPU1 learns it but mostly *spends* it through CPU2's warm-up
enrichment path, which is why it never shows up in CPU1's own multiplier.

That sampling discipline — throwing away the settling window, requiring a
majority, moving one step at a time — is the detail that most changed my
read of this ROM. Somebody thought hard about not learning garbage.

---

## Knock control

Retard is learned **per cylinder**, not globally, and persisted in
battery-backed PRAM across ignition cycles. CPU1 reads the knock MCU's 3-bit
level on every NE pulse, accumulates it across two crank positions, decodes
at the third, integrates per-cylinder retard, and ships the result to CPU2 to
be applied to the timing calculation.

Recovery is deliberately slow: retard decays by 2 counts every **256 ms**.
The decay function is called every 4 ms but gated behind a counter, which is
exactly the kind of thing that reads as a much faster re-advance than it is.

---

## Doing a lot with 16 KB

The ROM is full of tricks that only make sense when every byte counts.

**Self-re-arming one-shot gates.** `tbs` on this core is a *destructive*
test-and-set: it reports the bit's previous state and then sets it,
unconditionally. That makes a single instruction into a complete "has this
window elapsed" primitive — an ISR clears the bit at a fixed crank angle, the
first `tbs` after that both detects it and re-locks it, and every later test
in the same window correctly skips. No flag variable, no compare, no branch
to clear.

**Function fall-through.** `set_knock_sensor_err_flag`,
`check_knock_sensor_err_flag` and `negate_rD` are three separately-named
functions with no `ret` between them; each falls into the next. So "set the
flag" also negates D, and "check the flag" conditionally negates D. The
result is a disguised abs()-and-restore-sign idiom reused all over the
ROM — and it means `var_diag_errors_5.0` is usually a "did we negate"
remember-bit rather than anything to do with the knock sensor.

**Variable aliasing.** In one long span of the main loop the code snapshots
one flags variable, overwrites it with a *different* variable's value, runs a
block of bit-manipulation against it, and commits it back — reusing one
variable's instruction encodings for another's data. Bit tests inside that
span do not mean what their names say.

**Deliberate saturation override.** The 16×16 multiply helper clamps its own
result to `0xFFFF` on overflow. Several callers deliberately defeat that by
taking the high word instead, trading a coarser scale for keeping *some*
magnitude information rather than a flat pinned maximum. It is a hand-rolled
substitute for floating point, and it is used carefully.

---

## Where the era shows

Not in the architecture — in the arithmetic and the resolution.

Everything is hand-managed fixed point with manual scale tracking, and the
scale factors are load-bearing: misread one multiply and a term is out by 2×.
Tables are small; the long-term trim gets twelve cells to describe the whole
load range. There is no MAF. And disabled features were switched off by
zeroing a constant rather than removing the code — a permanently-zero
rev-limiter offset that makes its subtraction a no-op, a knock gate that is
only ever cleared so its branch can never be taken. Calibration by neutering.

---

## The verdict

The impressive thing is not any single feature — it is the **discipline**.
The control theory is current: PI control with a deadband, dual-rate adaptive
trim, per-cylinder knock learning, angle-domain scheduling in hardware. The
statistical hygiene in the cruise trim would not embarrass a modern
calibration. And the ROM-space idioms are the work of people who knew the
instruction set intimately and were counting cycles.

It reads like it was written by engineers who understood both control theory
and exactly what each cycle cost them, and who had to fit that into 16 KB
twice.

---

## A caveat on all of the above

The architecture and the interfaces are understood. **Roughly half the basic
blocks in CPU1 are still untraced**, and CPU2 is behind that. Every claim
here comes from code that has actually been read, but "not documented" in
this repo does not mean "not important" — it frequently means "not looked at
yet."

Read `session_journal.md` for current status before starting new work; it
carries the pending list, and entries later found to be wrong are annotated
in place rather than deleted.

## Where to go next

| Document | Covers |
|---|---|
| `fuel_calculation_system.md` | Injector pulse-width chain, the trims, the DMA load terms |
| `ignition_system.md` | CPR scheduling, dwell, advance blending, misfire detection |
| `knock_sensor_system.md` | Knock MCU protocol, per-cylinder retard learning |
| `idle_control_system.md` | ISCV target calculation, idle trim learning |
| `adc_system.md` | Channel scanning, sensor scaling, counter tick rates |
| `toshiba-8x-technical-reference.md` | Instruction set, opcode matrix, registers |
| `session_journal.md` | Chronological progress log and pending work |

---

*Derived from IDA disassembly of D151803-9651 and D151803-9661.*
