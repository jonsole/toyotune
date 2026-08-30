# The 3S-GTE Gen 3 ECU — an overview

**Toyota part 89861-17460 · Denso `D151803-9651` + `D151803-9661` · JDM SW20 MR2, 1993–94**

This is the front door to the rest of `roms/docs/`. It describes what the ECU
is, how it is put together, and which parts are worth a second look. The
per-subsystem write-ups go deeper; this one is meant to be read start to
finish, and every section ends with a pointer into them.

Code shown here is quoted from the annotated working copies —
`roms/3S-GTE/D151803-9651/Claude/D151803-9651.asm` (CPU1) and
`.../D151803-9661/Claude/D151803-9661.asm` (CPU2). Symbol names are stable;
line numbers are not, so search by label.

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
| Timer | 19-bit, 4 µs per count (TIMERC/8) |
| Crank resolution | 24 NE pulses/rev, 15° apart |
| Inter-CPU link | 1 MHz synchronous serial, DMA, 38-byte frame every 4 ms |
| Fuelling | Speed-density (VE × MAP × RPM), no MAF |
| Instruction set | M68HC11-derived, Denso-specific extensions |

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

**CPU1 (`-9651`) owns everything with a deadline.** Ignition scheduling
(`iv6_ne_process`, `int_vector_9_ignition`), injector firing
(`injector_update`, `injector_drive`), the ADC scan
(`int_vector_1_serial_rx` and 14 per-channel handlers), idle valve control
(`calc_iscv`), knock integration (`knock_mcu_update`, `knock_processing`),
and the whole closed-loop lambda trim (`update_lambda_stft`).

**CPU2 (`-9661`) owns the arithmetic.** The base VE maps, the speed-density
load term (`calc_params`), ignition timing candidates
(`calc_ignition_timing`), boost control via the TVSV duty cycle, and the OBD
datastream (`update_odb_flags`). It runs no lambda control of its own — it
only receives CPU1's O2 reading to pack a rich/lean bit into a diagnostic
byte.

**The knock MCU** is a third D8X in an SDIP64 package wired to the piezo
sensors. It hands CPU1 a 3-bit knock level over `PORTB` — bits 3 and 4 active
low, bit 5 active high — clocked against the crank. `PORTB.1` carries the TDC
cylinder-1 reference out to it, and CPU1 can hard-reset it by pulsing
`DOUT.2` low for about 12 µs.

The split is the most consequential design decision in the box. One 8-bit
part could not do speed-density arithmetic *and* hold hard real-time
deadlines, so Toyota paid for a second MCU, a second ROM, and an inter-CPU
protocol that has to be kept in sync. That is an expensive answer to a
compute wall, and they paid it.

> → `knock_sensor_system.md` § *Hardware Interface* for the PORTB pinout and
> reset timing; `session_journal.md` § *Architecture notes* for the CPU split.

---

## The inter-CPU link

The two CPUs share one physical DMA buffer, exchanged as a 38-byte frame
every 4 ms over a 1 MHz synchronous serial link — `ASR2` receive (`0x81DE`),
`ASR3` transmit (`0x9200`). Each CPU sees the same buffer at a **different
base address**, and the offset depends on direction:

| Direction | Formula | Confirmed by |
|---|---|---|
| CPU2 → CPU1 (CPU2 `dmatx_*` = CPU1 `dmarx_*`) | `CPU1 = CPU2 + 0xDA` | `dmarx_max_retard_23B_161` / `dmatx_max_retard_161` and two more pairs |
| CPU1 → CPU2 (CPU1 `dmatx_*` = CPU2 `dmarx_*`) | `CPU1 = CPU2 + 0x13B` | `dmatx_tps`/`dmarx_tps`, `dmatx_ect`/`dmarx_ect`, `dmatx_pim2`/`dmarx_pim2`, battery |

Getting these the wrong way round is the single easiest mistake to make when
cross-referencing the two disassemblies: applying `0xDA` to a `dmatx` address
lands inside CPU2's `var_serbus_rx` buffer rather than its DMA block, and
produces a confident-looking answer about an unrelated variable. It has
happened in this repo, and was caught only because the result was obvious
nonsense.

Two further wrinkles. Word-sized variables in one region carry a **one-byte
padding discrepancy** against the `0xDA` formula, so structural
position-matching is more reliable there than arithmetic. And not every field
in the frame is live: `dmarx_nv_trim_pim` — CPU1's learned barometric trim —
is written into CPU2's buffer every single frame by the bulk receive copy and
read by absolutely nothing.

> → `fuel_calculation_system.md` § *The pulse-width chain, end to end* for the
> DMA load terms; `session_journal.md` § *CPU1<->CPU2 DMA cross-reference*.

---

## Time and angle

Everything real-time hangs off a 19-bit hardware timer running at **4 µs per
count** (TIMERC divided by 8), and the crank gives 24 NE pulses per
revolution, one every 15°.

Ignition is computed in the NE interrupt as a *time offset* from the current
crank timestamp, then written into compare register `CPR0`. When the timer
matches, the hardware fires `int_vector_9_ignition`, which toggles the coil —
the CPU is not in the loop at the moment of the spark. `DOUT.0` low is
charging, high is firing; dwell is simply the interval between two `CPR0`
events, and the pending event is tracked by latching `DOM.0`.

Because 15° of crank is coarse for spark timing, advance is interpolated
*between* teeth. `ignition_timing_to_cpr` measures `ne_sum3` — the time for
the last 45°, three pulses — and converts:

```
1 crank degree = ne_sum3 / 45   timer units (4 µs each)
```

carrying a fractional remainder term (`var_ign_ne_frac`) so the result is not
quantised to whole teeth. Effective resolution is about **0.5° per count**.
Spark confirmation comes back from the igniter as IGF on `ASR0`, monitored
via `IRQLL.4`; missed events are counted by `check_IGF_error` and can trigger
fuel cut.

A hardware detail that matters when reading the ASM: **`ASR0` reads and
writes are different registers.** Writing configures DMA; reading returns the
timer value latched at an I/O transition. It cannot be read-modify-written,
which is why both CPUs keep software shadows (`var_asr0n_shadow_1DD` on CPU1,
`var_asr0n_shadow_126` on CPU2).

The ADC is external, reached over the serial port (`SIN0`/`SOUT0`) as a
multiplexer: CPU1 sends a channel select and receives the result of the
*previously* requested channel, so every reading is one cycle stale by
construction. Channels run on a priority schedule — TPS and TRAC-TPS every 8
ADC cycles, intake air temperature every 32, coolant in every low-priority
group.

> → `ignition_system.md` § *Timing Units* and § *`iv6_ne_process`*;
> `adc_system.md` § *ADC Channel Map* and § *Scan Phases*;
> `toshiba-8x-technical-reference.md` for the ASR0 asymmetry.

---

## Fuelling

CPU2 computes the load term — the base VE map (`map_c006_ve`) indexed by RPM
and MAP, multiplied by RPM and by MAP again, i.e. the classic
**VE × MAP × RPM** speed-density airflow estimate — and ships it to CPU1 as
`dmatx_ve_x_pim_x_rpm`. Two further correction terms travel with it: a
MAP-only VE correction (`table_ve_corr_map`) and a MAP × TPS bilinear
correction (`map_ve_corr_map_tps`), the latter forced to zero during idle
debounce.

CPU1 turns that into an injector pulse width through roughly seven stages:

| # | Stage | Where |
|---|---|---|
| 1 | Base width from load term, loop-mode selection, rate limit | `calc_inj_pw_base` |
| 2 | Fuel-trim multiplier (LTFT + STFT) | `apply_enrich_and_trims` @ `loc_E47B` |
| 3 | Ignition-blend term | `var_ign_blend_out` |
| 4 | Per-cylinder trim tables | `table_inj_pw_adj_C25B`/`C25F` |
| 5 | Limp-home fixed width | `loc_E729` |
| 6 | Optional halving of a full-cycle value | `var_ignition_flags.7` |
| 7 | Battery dead-time compensation (additive, per firing) | `var_inj_battery_adjust` |

Stage 6 is a good example of how easily a bit gets mislabelled. It is not an
"updated" flag, as its position among the status bits suggests — it means
*this value is a full-cycle width and must be halved before use*:

```asm
        tbbc    bit7, var_ignition_flags, loc_F2BE
        shr     d                     ; halve the pulse width
```

Two details worth carrying from stage 7: anything below `0x0D` (52 µs) is
rejected by `injector_drive` as too short to be worth firing, and if the
injector is already open the code *extends* the current pulse rather than
restarting it.

The asymmetry that trips people up is that the trims are multiplicative and
applied once centrally at stage 2, the per-cylinder correction at stage 4 is
a separate multiplicative stage with its own tables, and the battery
compensation at stage 7 is additive and per-event. "Adding fuel" means three
different operations depending on where you do it.

> → `fuel_calculation_system.md` § *The pulse-width chain, end to end* for the
> full trace, and § *The ramp-limiter cluster* for stage 1's blend logic.

---

## Three fuel trims

This is where the ECU is better than its era suggests.

### Short-term — a jump-and-ramp controller

`var_lambda_integrator` is plain RAM, never persisted, forced back to its
`0x8000` neutral on reset events. `update_lambda_stft` drives it with a
textbook proportional-plus-integral law. The proportional "jump" and the
deadband that surrounds it are visible in four instructions:

```asm
loc_DABF:
        ld      d, unk_1C4
        cmp     #0B3h, var_lambda_avg   ; rich threshold
        bcc     loc_DADB
        cmp     #4Dh, var_lambda_avg    ; lean threshold
        bgt     loc_DABA                ; 0x4E-0xB2 -> deadband, exit
        sub     d, #07AEh               ; lean: step the integrator
```

Between `0x4E` and `0xB2` the routine exits without acting — that deadband is
what stops the loop chattering around stoichiometric. Outside it the jump is
`0x07AE`; separately, a `±0x0010` per-tick *ramp* follows the sensor's sign,
each direction gated on the integrator not already sitting at its `0x1A00` /
`0xE600` rail.

### Long-term — learned per load site

`nv_afr_trim_base` is 12 bytes in battery-backed NV RAM, indexed by `var_pim2`
(manifold pressure, i.e. load) with interpolation between cells.
`read_nv_afr_trim` returns the first/idle cell directly when the throttle is
closed, and a neutral `0x80` when the trims are marked invalid.

The two are summed into one multiplier and applied once:

```asm
loc_E47B:
        push    x                       ; save the incoming pulse width
        jsr     read_nv_afr_trim        ; B = LTFT for this load cell (0x80 = neutral)
        clr     a
        add     b, var_lambda_integrator ; + STFT, the fast O2 integrator
        addc    a, #00h
        add     d, #0100h               ; + unity, so D = 1.0 + LTFT + STFT
        st      d, var_temp_w
        mov     s, x                    ; X = SP  (mov is src,dest)
        ld      x, x + 00h              ; X = the saved pulse width
        jsr     mult_rDrX_saturate      ; PW * (unity + LTFT + STFT)
```

### And a third, which isn't in that multiplier at all

`closed_loop_control` learns a slow global correction under tightly-gated
cruise only: coolant 82.9–103.8 °C, off idle, RPM below 3200, battery at
least 11.4 V, no transient in progress, CPU2 not requesting trim or
enrichment, and `var_trim_state == 4`.

Inside that window it takes 17 O2 samples counted by `var_o2_vote_cnt`, votes
±1 per sample into `var_o2_vote_accum` from a `0x80` neutral, **discards the
first seven as settling**, then applies a majority verdict with a `0x7D`–`0x83`
deadband to move the stored value by exactly one step. It is kept as a
value-and-complement pair using the same integrity scheme as
`write_rB_nv_ram`.

CPU1 learns it but mostly *spends* it through CPU2: it travels as
`dmatx_nv_trim_o2`, and CPU2's `check_startup` uses it as a multiplier on an
ECT-indexed table to build the warm-up enrichment that
`decay_enrichment_unk_100` then bleeds away. That is exactly why it never
appears in CPU1's own multiplier.

That sampling discipline — throwing away the settling window, requiring a
majority, moving one step at a time — is the detail that most changed my read
of this ROM. Somebody thought hard about not learning garbage.

> → `fuel_calculation_system.md` § *Short-term vs long-term fuel trim*.

---

## Knock control

Retard is learned **per cylinder**, not globally, and persisted in
battery-backed PRAM across ignition cycles.

`knock_mcu_update` runs on every NE pulse — 24× per revolution — and
accumulates the knock MCU's 3-bit level into `var_knock_info` across two
crank positions, decoding at the third. `knock_processing` then integrates
per-cylinder retard, indexed by `var_knock_cyl_idx`, and the result is sent to
CPU2 as `dmatx_ign_corr_cpu2` to be applied to the timing calculation.

Recovery is deliberately slow: `knock_retard_decay` reduces `var_knock_retard`
by 2 counts every **256 ms**. The function is *called* every 4 ms but only
acts once `var_cnt_knock_decay` reaches `0x40` — precisely the kind of
construction that reads as a much faster re-advance than it is, and one this
repo got wrong for a while.

One knock path is disabled rather than removed: `var_knock_gate_168` is only
ever cleared and has no setter, so the branch guarding it can never be taken.

> → `knock_sensor_system.md` § *`knock_mcu_update`* and
> § *Per-Cylinder Retard Integration*; also § *`var_diag_errors_5.0` is not a
> knock flag*, which matters for reading the rest of the ROM.

---

## Idle control

`calc_iscv` computes the ISC valve duty target by summing five
flare/enrichment terms plus a load-switch-selected offset, producing
`var_iscv_target_rpm`; the RPM error against it becomes
`var_iscv_rpm_cmp_197`, consumed throughout the idle path. The valve is
driven from `CPR1`/`DOUT.1` — a zero target skips the pulse entirely.

Two things make this subsystem harder to read than it looks. First,
`calc_iscv` can be bypassed altogether by a fixed-opening override selected by
`var_flags_46.6`. Second, it is not called every 4 ms despite living in that
dispatch — the gate is a single `tbs`:

```asm
loc_D380:
        tbs     bit1, var_flags_44
        bne     loc_D3A5
        jsr     calc_iscv
```

There is no separate setter for that bit; the test sets it (see below).
`bg_ne_process_F3BE` clears it every 8th NE tooth, so `calc_iscv` recomputes
once per fixed crank-angle window.

There is also a learned idle trim in NV RAM, gated on idle being stable for
about **9.8 seconds** — a threshold of `0x99` on a 64 ms counter, not the
612 ms it would be on a 4 ms one.

> → `idle_control_system.md` § *`calc_iscv` — Structure*, § *Fixed-Opening
> Override* and § *The idle-trim gate*.

---

## Doing a lot with 16 KB

The ROM is full of tricks that only make sense when every byte counts.

### Self-re-arming one-shot gates

`tbs` on this core is a **destructive** test-and-set: it reports the bit's
previous state and then sets it, unconditionally. That makes a single
instruction into a complete "has this window elapsed" primitive — an ISR
clears the bit at a fixed crank angle, the first `tbs` after that both detects
it and re-locks it, and every later test in the same window correctly skips.
No flag variable, no compare, no branch to clear it again. The `loc_D380`
snippet above is the whole mechanism.

Reading `tbs` as a non-destructive test — which this repo did for a while —
makes such gates look like they can never fire.

### Function fall-through

`set_knock_sensor_err_flag`, `check_knock_sensor_err_flag` and `negate_rD` are
three separately-named functions with no `ret` between them. Each falls
straight into the next:

```asm
set_knock_sensor_err_flag:
        setb    bit0, var_diag_errors_5
;       (no ret — falls through)
check_knock_sensor_err_flag:
        tbbc    bit0, var_diag_errors_5, locret_C4F0
;       (falls through)
negate_rD:
        neg     a
        neg     b
        subc    a, #00h
locret_C4F0:
        ret
```

So "set the flag" also unconditionally negates D, and "check the flag"
negates D only if an earlier call in the same computation set it. The result
is a disguised abs()-and-restore-sign idiom, reused all over the ROM — which
means **`var_diag_errors_5.0` is usually a "did we negate" remember-bit**, not
anything to do with the knock sensor. `calc_dmatx_pim` was long mistaken for a
knock or boost limiter purely because it calls these two functions; it is
manifold-pressure transient compensation.

### Variable aliasing

In one long span of the main loop (roughly `0xD931`–`0xE380`) the code
snapshots `var_flags_4E`, overwrites it with `var_trim_state`'s value, runs a
block of `tbbc`/`tbbs`/`setb`/`clrb` against it, then commits it back —
reusing one variable's instruction encodings for another's data. Bit tests
inside that span do not mean what their names say. A zero-byte `.equ` alias
(`var_trim_state_alias`) was added to make the reads honest.

### Deliberate saturation override

`mult_rDrX` clamps its own `D` output to `0xFFFF` whenever the high word is
nonzero. Several callers defeat that on purpose with `mov x, d`, taking the
high word instead — trading a coarser scale for keeping *some* magnitude
information rather than a flat pinned maximum. It is a hand-rolled substitute
for floating point, and it is used carefully.

Note the direction trap in that last one: **`mov` is `src, dest`**, the
opposite of `ld`/`st`. `mov x, d` means `D = X`. Misreading it changes
`update_ign_timing_blend`'s scale factor by 2×.

> → `fuel_calculation_system.md` § *Critical: `mov` operand direction* and
> § *Critical: the `var_flags_4E` / `var_trim_state` alias`*;
> `toshiba-8x-technical-reference.md` § *`tbs` instruction*.

---

## Where the era shows

Not in the architecture — in the arithmetic and the resolution.

Everything is hand-managed fixed point with manual scale tracking, and the
scale factors are load-bearing: misread one multiply and a term is out by 2×.
Tables are small; the long-term trim gets twelve cells to describe the whole
load range. There is no MAF. And disabled features were switched off by
zeroing a constant rather than removing the code — `unk_14A` is permanently
zero, making its subtraction in the rev-limiter hysteresis check a no-op;
`var_knock_gate_168` is only ever cleared. Calibration by neutering.

---

## The verdict

The impressive thing is not any single feature — it is the **discipline**.
The control theory is current: PI control with a deadband, dual-rate adaptive
trim, per-cylinder knock learning, angle-domain scheduling handed to
hardware. The statistical hygiene in the cruise trim would not embarrass a
modern calibration. And the ROM-space idioms are the work of people who knew
the instruction set intimately and were counting cycles.

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

---

## Where to go next

| Document | Covers |
|---|---|
| `fuel_calculation_system.md` | Pulse-width chain, the trims, DMA load terms, the `mov` and aliasing traps |
| `ignition_system.md` | CPR scheduling, dwell, advance blending, misfire detection |
| `knock_sensor_system.md` | Knock MCU protocol, per-cylinder retard, the abs() idiom |
| `idle_control_system.md` | ISCV target calculation, fixed-opening override, idle trim |
| `adc_system.md` | Channel map, scan phases, sensor scaling, counter tick rates |
| `toshiba-8x-technical-reference.md` | Instruction set, opcode matrix, registers, ASR0 asymmetry |
| `knock_mcu_update.ASM` | A fully-annotated routine kept as the annotation-style reference |
| `session_journal.md` | Chronological log, pending work, and corrections to earlier claims |

---

*Derived from IDA disassembly of D151803-9651 and D151803-9661.*
