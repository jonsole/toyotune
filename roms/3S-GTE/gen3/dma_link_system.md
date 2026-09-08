# Inter-CPU DMA link — CPU1 → CPU2 (D151803-9651 → D151803-9661)

The two Denso CPUs exchange a fixed block of RAM over a 1 MHz synchronous
serial link, driven by the 8X's serial DMA engine, once per 4 ms tick. This
document traces one direction end to end: what CPU1 puts in the block, how
the transfer is armed and detected on both ends, where each byte lands on
CPU2, and who consumes it there. Every claim below was read from the two
`Claude/` disassemblies; nothing is taken from symbol names alone.

The reverse direction (CPU2 → CPU1) is **not** covered here yet. Its CPU1-side
`dmarx_*` names are known to be one slot out — see the warning block at the
head of that block in `Claude/D151803-9651.asm`, and `session_journal.md`
§ *All four DMA offsets from hardware* — and will be documented once they are
corrected.

---

## 1. The registers

Both ends program the same two 16-bit registers, `ASR2` and `ASR3`, which on
this "enhanced" 8X variant are the serial DMA engine rather than the base
variant's edge-capture timers (`toshiba-8x-technical-reference.md` § *ASR
registers*). Each is written as `mode_nibble | buffer_address`:

| CPU | register | value | meaning |
|-----|----------|-------|---------|
| CPU1 | `ASR2` | `0x8000 + var_dma_rx_buffer` (`0x81DE`) | receive: land the incoming frame at `0x1DE` |
| CPU1 | `ASR3` | `0x9000 + dmatx_pim2` (`0x9200`) | transmit: stream from `0x200` |
| CPU2 | `ASR2` | `0x9000 + var_serbus_rx` (`0x9127`) | receive: land the incoming frame at `0x127` |
| CPU2 | `ASR3` | `0x8000 + dmatx_ve_corr_map` (`0x814D`) | transmit: stream from `0x14D` |

**The nibble is not "RX vs TX".** CPU1's transmit and CPU2's receive — the two
ends of *this* direction — both use `9`; the two ends of the reverse direction
both use `8`. So the nibble identifies which of the two physical channels a
register is attached to, and both CPUs program the same ID for the same
channel. What the hardware does with it (clock source, pin select) is not
established.

**Transmit has no packing buffer.** `ASR3` points straight at the first live
`dmatx_*` variable and the engine streams RAM from there. The block is
therefore not a snapshot: a field written between the engine reading byte *n*
and byte *n+1* goes out torn across two frames. Individual 16-bit fields are
safe because they are written with `st d`, a single instruction, but nothing
guarantees consistency *between* fields.

---

## 2. CPU1: assembling and sending the block

### 2.1 The block

38 bytes at `0x200`–`0x225`. Its start is `dmatx_pim2`, which is exactly
what `ASR3` is programmed with (`ld d, #9000h + dmatx_pim2` at
`reset_vector` and in `start_dma`).

Most fields are filled by two mechanisms, with a handful written from
elsewhere:

- **The ADC handlers write their own field as they run.** Each is written
  in the handler that produced it, so it is as fresh as the last scan of that
  channel: `adc_handler_pim_ok` → `dmatx_pim2`, `adc_handler_tps_high` →
  `dmatx_tps`, `adc_handler_ect` → `dmatx_ect`, `adc_handler_tha` →
  `dmatx_tha`, `adc_handler_tham` → `dmatx_tham`, `adc_handler_battery` →
  `dmatx_battery`, `adc_handler_throttle_closed` → `dmatx_adc_lambda`.
- **`copy_dma_tx` marshals the rest every 4 ms.** Called from
  `iv6_4ms_process` (and once at init). It is straight-line: load a source
  variable, store it to its slot. When the NV trims are not yet valid
  (`var_flags_42.0` clear) it substitutes fixed defaults — `0x50` for
  `dmatx_nv_trim_pim`, `0x00` for `dmatx_nv_trim_o2` — rather than sending
  garbage. It also *builds* `dmatx_flags_1` bit by bit from six unrelated
  CPU1 conditions (see the table).
- **Written from their own subsystems:** `dmatx_pim` by `calc_dmatx_pim`,
  `dmatx_obd_inj`/`dmatx_obd_iscv` by `update_diag_obd`, `dmatx_obd_o2_sensor`
  by the OBD output code, `dmatx_ign_obd` by `bg_ne_process`,
  `dmatx_ign_corr_cpu2` by `knock_processing`, `dmatx_knock_retard` by
  `check_clear_speed_limiter_rev`.

### 2.2 Kicking the transfer: `start_dma`

`start_dma` runs from `iv6_4ms_process` (its "step 4") every tick, and from
init. It is a state machine over `TIMER3`, which on this variant is not the
base part's 3-bit timer LSB but a status/control byte for the DMA engine —
it is written with full 8-bit values (`0xB7`, `0x4F`, `0xF9`) and read back
with `cmpb`, i.e. **bit-tested**, never compared as a number:

```
ld   a, TIMER3
cmpb a, #40h        ; bit 6 set  -> loc_F8AB (skip the TX kick)
beq  loc_F8AB
cmpb a, #08h        ; bit 3 clear -> loc_F899 (kick TX)
bne  loc_F899
```

Two arming sequences follow. **Transmit** re-programs `ASR3` with the block
address and writes `TIMER3 = 0xB7`; **receive** re-programs `ASR2` with the
landing buffer and writes `TIMER3 = 0x4F`. Around each, `ASR0N` bits 6 and 7
are toggled through a shadow copy (`var_asr0n_shadow_1DD`) — `ASR0N` is
evidently repurposed as an enable on this variant, which bits mean what is
not established. Retry counting is done with `var_cnt_unk_76`/`_77` and
`var_4ms_cnt_C4`/`_C5`; after `0x24` failed polls it drops the IV0 enable
(`IMASKL.2`) and sets `TIMER3.7`, and after three re-arms sets
`var_flags_46.7` as a "link not syncing" flag.

Nothing here is edge-triggered by serial activity. It is a fixed-rate poll
and re-arm loop hung off the 4 ms tick.

### 2.3 Receive completion: `IV0`

Vector `0xFFDE`, enabled by `IMASKL.2`, cleared by `IRQLL.2`. It reads
`TIMER3` again and treats `TIMER3 & 0x30 != 0` together with `RAMST.2` clear
as "frame received": it then calls `copy_dma_rx` **inside the interrupt**,
which word-copies 34 bytes from `var_dma_rx_buffer` into the `dmarx_*`
block, and zeroes the retry counters. Otherwise it saturating-increments
`var_cnt_unk_76`. This is CPU1's receive side of the *reverse* direction; it
matters here only for the asymmetry noted in §3.3.

---

## 3. CPU2: receiving and unpacking

### 3.1 Arming

`main_loop` programs `ASR2 = 0x9127` (receive into `var_serbus_rx` at
`0x127`, 35 bytes) and `int_vector_0` re-arms it with the same constant on
every tick, unconditionally, together with `TIMER3 = 0x4F` — the same
receive-arm constant CPU1 uses.

### 3.2 Completion: `int_vector_0`

The same idiom as CPU1's `IV0`, byte for byte: `TIMER3 & 0x30` with `RAMST.2`
clear means a frame has landed. On sync it clears the retry counters and
**sets `var_flags_47.5`** ("frame ready"); otherwise it clears that flag and
saturating-increments `var_dma_sync_timeout_55`.

### 3.3 Unpacking: `copy_serbus_rx`

Unlike CPU1, CPU2 does not unpack in the interrupt. `main_loop` tests
`var_flags_47.5`, calls `copy_serbus_rx` if it is set, and clears the flag.
So CPU2 sees each frame once, on the next main-loop pass, rather than the
moment it lands.

`copy_serbus_rx` has two parts, and **the second is why a single offset does
not describe this direction**:

1. **Fifteen words copied verbatim** from `var_serbus_rx[0..0x1D]` to
   `dmarx_pim2` (`0xC5`) onward, i.e. `0xC5`–`0xE2`. For these 30 bytes the
   relationship is exactly `CPU1_addr = CPU2_addr + 0x13B`, which is also what
   the hardware gives (`0x200 − 0xC5`).
2. **Four explicit single copies** of the tail into CPU2's low flag area:

   | frame offset | CPU1 source | CPU2 destination |
   |---|---|---|
   | `+0x1E`–`0x1F` | `dmatx_error_flags1`/`_2` (`0x21E`–`0x21F`) | `dmarx_unk_4B` (`0x4B`, 2 bytes) |
   | `+0x20` | `dmatx_flags_46` (`0x220`) | `dmarx_var_flags_46` (`0x42`) |
   | `+0x21` | `dmatx_flags_1` (`0x221`) | `dmarx_flags_1` (`0x4E`) |
   | `+0x22` | `dmatx_limiter_flags` (`0x222`) | `dmarx_limiter_flags` (`0x43`) |

   Applying `+0x13B` to any of these gives a wrong address.

Frame bytes `0x23`–`0x25` (CPU1's `unk_223`/`word_224`) are received into
`var_serbus_rx` but never copied out: CPU2 does not consume them.

---

## 4. Field by field

Offsets are into the frame; "CPU1 writer" is the routine that stores the
value; "CPU2 readers" are the routines that reference the landed variable
(none of these are name-derived — they are the actual `ld`/`cmp`/`tbbs`
sites). Sizes in bytes.

| off | CPU1 addr | CPU1 name | sz | CPU1 writer / source | CPU2 addr | CPU2 name | CPU2 readers |
|----:|-----------|-----------|---:|----------------------|-----------|-----------|--------------|
| 00 | `0x200` | `dmatx_pim2` | 2 | `adc_handler_pim_ok` | `0xC5` | `dmarx_pim2` | ignition, `calc_params`, `open_loop`, `drive_DOUT0`/`DOUT2`, VE/knock tables (15 sites) |
| 02 | `0x202` | `dmatx_tps` | 2 | `adc_handler_tps_high` | `0xC7` | `dmarx_tps` | `calc_ignition_timing`, `open_loop`, `drive_DOUT0`, `map_ve_corr_map_tps` |
| 04 | `0x204` | `dmatx_ect` | 2 | `adc_handler_ect` (`st d, var_ect` then `st d, dmatx_ect`) | `0xC9` | `dmarx_ect` | 20+ sites: ignition, every ECT table, enrichment decay, `drive_DOUT0`/`DOUT2` |
| 06 | `0x206` | `dmatx_inj_pw_inj1` | 2 | `copy_dma_tx` ← `var_inj_pw_inj1` | `0xCB` | `dmarx_inj_pw_inj1` | `drive_DOUT2_tvsv` |
| 08 | `0x208` | `dmatx_pim` | 2 | `calc_dmatx_pim` | `0xCD` | `dmarx_pim` | `calc_ignition_timing`, `calc_params`, `map_c006_ve` |
| 0A | `0x20A` | `dmatx_tha` | 1 | `adc_handler_tha` | `0xCF` | `dmarx_tha` | enrichment decay, `drive_DOUT0`, THA tables |
| 0B | `0x20B` | `dmatx_tham` | 1 | `adc_handler_tham` | `0xD0` | `dmarx_tham` | `calc_ignition_timing`, `drive_DOUT2_tvsv`, `table_C3BB_tham` |
| 0C | `0x20C` | `dmatx_battery` | 1 | `adc_handler_battery` | `0xD1` | `dmarx_battery` | `drive_DOUT2_tvsv` |
| 0D | `0x20D` | `dmatx_nv_trim_pim` | 1 | `copy_dma_tx` ← `var_nv_trim_unk_98`, else `0x50` | `0xD2` | `dmarx_nv_trim_pim` | **none found** — see §5 |
| 0E | `0x20E` | `dmatx_cmd_startup_20E` | 1 | `copy_dma_tx` ← `var_cnt_startup` | `0xD3` | `dmarx_cnt_startup` | `calc_params`, `check_startup` |
| 0F | `0x20F` | `dmatx_cnt_unk_20F` | 1 | `copy_dma_tx` ← `var_cnt_EA` | `0xD4` | `dmarx_unk_D4` | `calc_ignition_timing`, enrichment decay, `drive_DOUT2_tvsv` |
| 10 | `0x210` | `dmatx_nv_trim_o2` | 1 | `copy_dma_tx` ← `var_nv_trim_unk_96`, else `0x00` | `0xD5` | `dmarx_nv_trim_o2` | `decay_enrichment_unk_FE`, `table_C393` |
| 11 | `0x211` | `dmatx_lambda_state` | 1 | `copy_dma_tx` ← `var_lambda_state` | `0xD6` | `dmarx_lambda_state` | `calc_params`, `main_continue` |
| 12 | `0x212` | `dmatx_adc_lambda` | 1 | `adc_handler_throttle_closed` | `0xD7` | `dmarx_adc_lambda` | `update_odb_flags`, OBD output |
| 13 | `0x213` | `dmatx_knock_retard_info` | 3 | `copy_dma_tx` ← `nv_table_knock_info` (+2) | `0xD8` | `dmarx_knock_info` | `drive_DOUT0` |
| 16 | `0x216` | `dmatx_ign_corr_cpu2` | 1 | `knock_processing` ← `dmarx_knock_retard_cpu2` | `0xDB` | `dmarx_add_enrichment_DB` | `update_odb_flags` (non-zero test only) — **see §5** |
| 17 | `0x217` | `dmatx_obd_inj` | 1 | `update_diag_obd` | `0xDC` | `dmarx_obd_inj` | `next_odb_byte` via `table_odb` — see §5 |
| 18 | `0x218` | `dmatx_ign_obd` | 1 | `bg_ne_process` | `0xDD` | `dmarx_obd_ign` | `next_odb_byte` via `table_odb` — see §5 |
| 19 | `0x219` | `dmatx_obd_iscv` | 1 | `update_diag_obd` | `0xDE` | `dmarx_obd_iscv` | `next_odb_byte` via `table_odb` — see §5 |
| 1A | `0x21A` | `dmatx_obd_o2_sensor` | 1 | OBD output code | `0xDF` | `dmarx_obd_o2_sensor` | `next_odb_byte` via `table_odb`, `output_odb_bit` |
| 1B | `0x21B` | `dmatx_knock_retard` | 1 | `check_clear_speed_limiter_rev` | `0xE0` | `dmarx_knock` | `table_knock_enrichment`, `main_continue_2` |
| 1C | `0x21C` | `dmatx_pw_loop_mode` | 1 | `copy_dma_tx` ← `var_pw_loop_mode` | `0xE1` | `dmarx_dout0_duty_E1` (**2 bytes**) | `drive_DOUT0` — **see §5** |
| 1D | `0x21D` | `dmatx_tps_delta` | 1 | **no writer found** | `0xE2` | (low byte of the above) | |
| 1E | `0x21E` | `dmatx_error_flags1` | 1 | `copy_dma_tx` ← `var_error_flags1` (`st d`, both bytes) | `0x4B` | `dmarx_unk_4B` (2) | `drive_DOUT0` |
| 1F | `0x21F` | `dmatx_error_flags2` | 1 | (second byte of the above) | `0x4C` | | |
| 20 | `0x220` | `dmatx_flags_46` | 1 | `copy_dma_tx` ← `var_flags_46` | `0x42` | `dmarx_var_flags_46` | 16 sites — the most-read byte in the frame |
| 21 | `0x221` | `dmatx_flags_1` | 1 | `copy_dma_tx`, built bitwise (below) | `0x4E` | `dmarx_flags_1` | `calc_params`, `factory_selfcheck`, OBD, clamp tables |
| 22 | `0x222` | `dmatx_limiter_flags` | 1 | `copy_dma_tx` ← `var_limiter_flags` | `0x43` | `dmarx_limiter_flags` | `drive_DOUT0` |
| 23–25 | `0x223`–`0x225` | `unk_223`, `word_224` | 3 | — | — | not copied out | — |

**`dmatx_flags_1`**, built in `copy_dma_tx`:

| bit | set when |
|-----|----------|
| 0 | `var_flags_4E_copy_2 & 0x80 == 0` |
| 1 | `var_flags_4F_saved & 0x04 == 0` |
| 2 | `var_io_input1.1` set (throttle closed / IDL) |
| 3 | `var_flags_4E_copy2 & 0x01 == 0` |
| 4 | `var_flags_4D.2` set |
| 5 | `var_io_input1.3` set |

The name spellings differ between the two sides at a few slots
(`cmd_startup`/`cnt_startup` — the CPU1 spelling is surely a typo for "cnt";
`knock_retard_info`/`knock_info`; `ign_obd`/`obd_ign`; `knock_retard`/`knock`).
These are cosmetic: the slot is the same on both sides.

---

## 5. Things that do not add up

Recorded rather than resolved.

**One field is sent and never read: `nv_trim_pim`.** No reader for
`dmarx_nv_trim_pim` was found on CPU2. `ecu_overview.md` describes it as a live
term in the frame; on this evidence it is transmitted and ignored. As always,
an indexed read would not show up — but see the correction immediately below
for how easily that conclusion goes wrong.

**Correction — the three OBD fields *are* read, via a table of addresses.**
An earlier draft of this document listed `obd_inj`, `obd_ign` and `obd_iscv`
as unread. They are not: they are consumed by the VF-pin diagnostic datastream
(§6), which reaches them through `table_odb`, a table of *addresses* rather
than values. Two things made that easy to miss, and both are worth knowing
before trusting a "no reader" claim:

- The reference is a `.dw` operand, not a `ld`. A search for instructions
  touching the symbol finds nothing.
- The scan that produced the original claim required a line to begin with a
  tab, so it silently skipped the one line in the table that also carries the
  `table_odb:` label — which is precisely the line those three appear on.
  `dmarx_obd_o2_sensor` sits on a continuation line and *was* found, which is
  why the first three looked unread and the fourth did not. A "no reader"
  result that splits a group like that should be treated as a bug in the
  search, not a finding.

**Slot `0x216`: CPU1 echoes a CPU2 byte back.** `knock_processing` writes
`dmatx_ign_corr_cpu2` from `dmarx_knock_retard_cpu2` — a value that *came
from CPU2* in the previous frame — so this slot is a loopback, not a CPU1
computation. CPU2 reads it only in `update_odb_flags`, as a non-zero test
feeding an OBD status bit. Neither name is supported by its own side's code:
CPU1's says "ignition correction", CPU2's says "add enrichment", and what
actually travels is CPU2's own value coming back. (Note `dmarx_knock_retard_cpu2`
is itself in the reverse-direction block whose names are one slot out.)

**Slot `0x21C`–`0x21D`: the two sides disagree about the width.** CPU1 sends
two independent bytes, `var_pw_loop_mode` at `0x21C` and `dmatx_tps_delta` at
`0x21D` — and no writer of `dmatx_tps_delta` was found, so the second byte may
be stale. CPU2's `drive_DOUT0` reads `0xE1`–`0xE2` as a **single 16-bit**
"DOUT0 duty". One of these readings is wrong, and it is the one that drives a
physical output. Worth settling from `drive_DOUT0`'s arithmetic.

**CPU2 receives 35 bytes; CPU1 sends 38.** `var_serbus_rx` is 35 bytes and
`copy_serbus_rx` consumes exactly 35. Whether the engine actually transfers
38 and CPU2 drops the tail, or the count is 35 and CPU1's last three bytes
never leave, is not visible from the software side.

---

## 6. Where the OBD fields go: the VF-pin datastream

Three of the frame's fields exist purely to be serialised back out of the ECU
on CPU2's diagnostic output, so the CPU1 → CPU2 link is the middle leg of a
longer path: CPU1 measures, DMA carries it over, CPU2 shifts it out of a pin
for a workshop tester.

**Activation.** `check_io_inputs` reads PORTB.7 into `var_input_bits` bit 1,
setting the bit when the pin reads **low** — i.e. when the diagnostic
connector's **TE2 terminal is jumpered to E1**. Everything below is gated on
that bit.

**The two mutually exclusive uses of PORTA.4.** That single pin is Toyota's
**VF** diagnostic terminal, and it carries one of two things depending on the
same `var_input_bits.1` gate:

- **TE2 open** — `generate_vf_PORTA_4` drives it as a slow PWM whose duty
  encodes `var_vf`, the classic analogue "read VF with a voltmeter" signal.
  It skips while the datastream is active.
- **TE2 grounded** — `output_odb_bit` instead shifts `var_odb_shift_reg` out
  one bit per `int_vector_c_timer` tick. It skips while the datastream is
  *not* active.

**What gets sent.** `next_odb_byte` runs every 4 ms from `iv6_4ms_process`,
walking `table_odb` two bytes at a time and loading each pointed-to value into
`var_odb_shift_reg`. The table holds **addresses**, and its eleven entries are,
in order:

| # | address | what it is |
|---|---------|-----------|
| 1 | `var_ne_table+1` | an NE period sample |
| 2 | `dmarx_obd_inj` | **from CPU1** — injector OBD snapshot (`update_diag_obd`) |
| 3 | `dmarx_obd_ign` | **from CPU1** — ignition OBD snapshot (`bg_ne_process`) |
| 4 | `dmarx_obd_iscv` | **from CPU1** — ISCV OBD snapshot (`update_diag_obd`) |
| 5 | `var_rpm_div_25` | RPM |
| 6 | `dmarx_pim2` | **from CPU1** — manifold pressure |
| 7 | `dmarx_ect` | **from CPU1** — coolant temperature |
| 8 | `dmarx_tps` | **from CPU1** — throttle position |
| 9 | `var_spd` | vehicle speed |
| 10 | `dmarx_obd_o2_sensor` | **from CPU1** — O2 sensor reading |
| 11 | `odb_null` | a fixed zero, presumably a frame delimiter |
| 12 | `var_obd_flags1` | status byte built by `update_odb_flags` |
| 13 | `var_odb_flags2` | status byte built by `update_odb_flags` |

Seven of the thirteen are values CPU1 supplied over this link. So the OBD
datastream is largely a CPU1 datastream that CPU2 merely transmits — which is
why those fields have a writer on CPU1 with an obvious purpose and no
*computational* consumer on CPU2 at all.

---

## 7. Related

- `session_journal.md` § *CPU2 (D151803-9661): serial_dma_start/int_vector_0's
  ASR2/ASR3/TIMER3 protocol decoded* — the original decode of the register
  format.
- `session_journal.md` § *All four DMA offsets from hardware* — why the reverse
  direction's names are one slot out, and the derivation of every offset.
- `CLAUDE.md` — the offset table and the rule that offsets come from the
  buffer registers, never from name pairs.
- `fuel_calculation_system.md` — how CPU1 consumes the VE terms that come back
  the other way.
