# Toshiba 8X MCU — Technical Reference (Part 2)

*This file is a continuation of Part 1: toshiba-8x-reference-part1.md*

---

## Appendix — Toyota 3S-GTE ECU Application Notes

The following is derived from annotated IDA disassemblies of two Toyota ECU ROMs (part numbers D151803-9651 and D151803-9661) for the **3S-GTE** engine. These notes are application-specific. The general dual-CPU architecture is described in the [Dual-CPU Architecture](#dual-cpu-architecture) section above.

This ECU uses the **enhanced variant** D8X with 8 compare registers and ASR2/ASR3 DMA. CPU1 handles real-time I/O and actuation; CPU2 handles fuelling, ignition calculations, and boost control.

### Serial DMA Protocol (CPU1 ↔ CPU2)

The inter-CPU link uses a DMA-style serial transfer. CPU2 RAM uses the naming convention `dmarx_` for data received from CPU1 and `dmatx_` for data transmitted to CPU1. The full packet structure per 4ms frame, inferred from variable names, is:

**CPU1 → CPU2 (sensor data):**

| Index | Variable | Content |
|---|---|---|
| DMARX00–01 | dmarx_pim2 | Normalised MAP sensor reading |
| DMARX02–03 | dmarx_tps | Throttle position |
| DMARX04–05 | dmarx_ect | Engine coolant temperature |
| DMARX06–07 | dmarx_word_CB | Unknown |
| DMARX08–09 | dmarx_pim | MAP sensor (raw) |
| DMARX0A | dmarx_tha | Intake air temperature |
| DMARX0B | dmarx_tham | Manifold air temperature |
| DMARX0C | dmarx_battery | Battery voltage |
| DMARX0E | dmarx_unk_D3 | Unknown |
| DMARX0F | dmarx_unk_D4 | Unknown |
| DMARX10 | dmarx_unk_D5 | Unknown |
| — | dmarx_adc_lambda | Lambda sensor ADC value |
| — | dmarx_knock_info | Knock information (3 bytes) |
| — | dmarx_obd_inj | OBD injector data |
| — | dmarx_obd_ign | OBD ignition timing |
| — | dmarx_obd_iscv | OBD ISCV duty |
| — | dmarx_obd_o2_sensor | OBD O2 sensor |
| — | dmarx_knock | Knock value |
| — | dmarx_limiter_flags | Limiter/rev-cut flags |
| — | dmarx_var_flags | Status flags |

**CPU2 → CPU1 (calculated outputs):**

| Variable | Content |
|---|---|
| dmatx_scaled_ve | Scaled volumetric efficiency |
| dmatx_rpm_x_5p12 | RPM × 5.12 |
| dmatx_warmup_enrichment | Warm-up fuel enrichment |
| dmatx_fuel_enrichment | Final fuel enrichment |
| dmatx_tham_enrich | Manifold air temp enrichment |
| dmatx_knock_unk | Knock-derived value |
| dmatx_max_retard | Maximum ignition retard |
| dmatx_ign_timing | Final ignition timing |
| dmatx_ign_timing_fallback1 | Fallback ignition timing (RPM table) |
| dmatx_map_table_unk | Map table result |

### CPU1 Port Signal Assignments (3S-GTE)

#### Port A ($20h) — Mixed I/O

| Bit | Direction | Signal | Notes |
|---|---|---|---|
| 0 | Output | Watchdog reset | Toggles every 4ms |
| 1 | Input | STA (starter signal) | High when starter running |
| 2 | Input | IDL (idle contact) | Low when throttle closed |
| 3 | — | Unknown | — |
| 4 | — | Unknown | — |
| 5 | Input | STP signal | High when 12V at pin |
| 6 | Input | G1 signal | Crank/cam sensor |
| 7 | Input | G2 signal | Crank/cam sensor |

#### Port B ($22h) — Knock MCU Interface + Diagnostics

| Bit | Direction | Signal | Notes |
|---|---|---|---|
| 0 | Output | Signal to knock MCU | Purpose TBD |
| 1 | Output | Cylinder 1 signal to knock MCU | — |
| 2 | — | Unknown | — |
| 3 | Input | Knock input LSB | From knock MCU |
| 4 | Input | Knock input | From knock MCU |
| 5 | Input | Knock input MSB | From knock MCU |
| 6 | Input | TE1 | Active low — diagnostic check mode |
| 7 | Input | TE2 | Active low — OBD stream enabled |

#### Port C ($28h) — Input only

| Bit | Signal | Notes |
|---|---|---|
| 0–1 | Unknown | — |
| 2 | PS/IDUP | Low when 0V at pin |
| 3–7 | Unknown | — |

#### Port D / ASRIN ($29h)

| Bit | Direction | Signal | Notes |
|---|---|---|---|
| 0 | Output | Air-con idle up VSV | — |
| 1 | Output | MIL (Malfunction Indicator Lamp) | 0 = LED on |
| 2 | Input | ELS pin | High when 12V at pin |
| 3 | Input | ECO pin | High when 12V at pin |
| 4–7 | Input | ASR pin status | Read-only |

#### DOUT Register ($26h) — Actuator Outputs

| Bit | Signal | Notes |
|---|---|---|
| 0 | Ignition IGT | Output |
| 1 | ISCV (Idle Speed Control Valve) | Output |
| 2 | Knock MCU reset??? | Uncertain |
| 3 | Unknown | — |
| 4 | Injector #1 | Output |
| 5 | Injector #2 | Output |
| 6 | Injector #3 | Output |
| 7 | Injector #4 | Output |

---

### CPU1 RAM Variable Map

#### Flags and State ($40h–$4Fh)

| Address | Name | Description |
|---|---|---|
| $40 | unk_40 | 40.1: startup injection pulse done; 40.3: transient throttle injection; 40.5: set when RPM < 300, cleared when RPM > 500 |
| $41 | var_schedule_flag | Flags controlling scheduling of various processes |
| $42 | var_flags_42 | 42.0: trim valid; 42.1: ADC table mode; 42.2: cleared in 4ms interrupt to schedule 4ms background; 42.3: cleared in NE interrupt; 42.7: set at end of 4ms process |
| $43 | var_limiter_flags | 43.3: speed limiter active; 43.4: 4 IGF pulses missing; 43.5: throttle shut; 43.6: rev limit active; 43.7: boost limit exceeded |
| $44 | unk_44 | — |
| $45 | var_ignition_flags | 45.7: set to halve injector pulse widths |
| $46 | var_flags_46 | 46.0: RPM < 200 (clr >400); 46.1: closed loop allowed; 46.2: throttle closed flag; 46.3: TPS flag; 46.4: diagnostic error; 46.5: knock sensor error; 46.7: sub-CPU error |
| $47 | unk_47 | — |
| $48 | var_diag_errors_5 | 48.0: RPM rising/falling; 48.3: sensor error; 48.4: knock management error; 48.5: air-con switch signal |
| $49 | var_io_input1 | 49.0: STA high; 49.1: IDL low (throttle closed); 49.4: STP high; 49.5: TE1 low (diagnostic); 49.6: TE2 low (OBD stream); 49.7: ELS high |
| $4A | var_io_input2 | 4A.0: ECO high; 4A.3: PS/IDUP |
| $4B | var_error_flags1 | 4B.1: G1/G2 error; 4B.2: too many IGF missing; 4B.3: ECT sensor error; 4B.4: THA error; 4B.5: PIM error; 4B.6: SPD error |
| $4C | var_error_flags2 | 4C.0: knock sensor error; 4C.1: TPS error; 4C.3: TRAC TPS error; 4C.4: THAM sensor error; 4C.5: O2 sensor heater error |
| $4D | var_flags_4D | 4D.3: using default PIM value; 4D.4: too many IGF missing; 4D.7: starter running during ignition |
| $4E | var_flags_4E | 4E.1: closed loop mode; 4E.2: ECT > 75°C; 4E.7: boost limit exceeded error |
| $4F | var_flags_4F | 4F.0: STA high; 4F.1: ECO high; 4F.3: ELS high; 4F.5: diagnostic mode |

#### Sensor Values ($50h–$7Fh)

| Address | Name | Description |
|---|---|---|
| $50 | var_pim2 | MAP sensor: (ADC − 10560) × 1.285156 |
| $52 | var_tps | Throttle position sensor value |
| $54 | var_tha | Intake air temperature |
| $55 | var_tham | Manifold air temperature |
| $56 | var_adc_battery | Battery voltage (ADC) |
| $57–$58 | var_ect | Engine coolant temperature (2 bytes) |
| $59–$5A | var_rpm_x_5p12 | Engine speed × 5.12 (2 bytes) |
| $5B | var_rpm_div_25 | Engine speed ÷ 25 |
| $5C | var_rpm_delta | RPM change delta |
| $5D | var_speed_kph | Road speed in km/h |
| $5E | var_tps_delta | TPS rate of change |
| $5F | var_adc_lambda | Lambda sensor voltage (signed) |

#### 4ms Counters ($ADh–$CFh)

| Address | Name | Description |
|---|---|---|
| $AD | var_4ms_cnt_AD | Initialised at ignition on; PORTA.0 starts toggling after 96ms (24 × 4ms) |
| $AE | var_4ms_cnt_sta | Incremented when starter is running |
| $B2 | var_4ms_cnt_speed | Speed update counter |
| $BA | var_4ms_cnt_starter | Count of 4ms periods starter has been running |
| $BB | var_4ms_cnt_lambda | Lambda feedback counter |
| $BC | var_4ms_boost_cnt | Increments when PIM < boost cut limit |
| $C0 | var_4ms_starter_cnt | — |
| $C1 | var_4ms_cnt_igf | IGF (ignition feedback) timer counter |
| $C2 | var_4ms_cnt_ne | Related to dwell calculation |
| $C3 | var_4ms_boost_cut_cnt | Counter of how long boost cut has been active |

#### NE / Ignition Tracking ($A0h–$AFh)

| Address | Name | Description |
|---|---|---|
| $A0 | var_diag_errors_4 | A0.2: speed sensor error; A0.3: starter signal error; A0.5: O2 heater error |
| $A1 | var_ne_count | NE counter: bits 0–3 = crank position (0→5), bits 4–7 = cylinder (0→3) |
| $A2 | var_ne_count_2 | Copy of var_ne_count |
| $A4 | var_ign_timing_temp | Temporary ignition timing value (used in NE interrupt) |
| $A7 | var_cnt_knock_signal | Knock signal count from knock MCU |

#### PRAM — Non-Volatile Storage ($80h–$9Fh)

| Address | Name | Description |
|---|---|---|
| $80–$81 | nv_diag_errors_1 | 80.0: G1/G2 signal; 80.1: NE signal; 80.2: IGT/IGF; 80.3: ECT; 80.4: THA; 80.5: turbo pressure; 80.6: speed sensor |
| $82–$83 | nv_diag_errors_2 | 82.0: knock; 82.1: primary TPS; 82.2: O2; 82.3: secondary TPS; 82.4: MAT; 82.5: O2; 82.6: lean error; 82.7: chargecooler pump |
| $84–$85 | nv_diag_errors_3 | 84.0: boost pressure |
| $86–$91 | nv_unk_trim | Trim values (12 bytes) |
| $94 | nv_unk_trim_94 | Trim value |
| $95 | nv_unk_trim_95 | Trim value |
| $96 | var_nv_trim_96 | — |
| $98 | var_nv_trim_98 | PIM value at start |
| $9A | var_nv_idle_trim | Idle trim value |
| $9C–$9D | var_nv_valid | Magic value `5AA5h` if PRAM data block is valid |
| $9E–$9F | unk_9E / unk_9F | — |

---

### CPU1 Key Subroutines (known)

| Label | Description |
|---|---|
| `reset_vector` | Reset / boot entry point |
| `iv6_4ms_process` | IV6 interrupt: main 4ms background process |
| `iv6_ne_process` | NE (crank) interrupt handler — ignition timing |
| `int_vector_9_ignition` | IV9: ignition dwell / IGT drive |
| `int_vector_4_kph` | IV4: speed sensor pulse counting |
| `injector_drive` | Injector pulse width output |
| `drive_dout1_iscv` | ISCV (idle speed control valve) drive |
| `async_throttle_inject` | Asynchronous throttle-tip injection |
| `start_dma` | Serial DMA / inter-CPU data transfer |
| `knock_mcu_update` | Reads knock data from knock MCU |
| `check_boost_limit` | Boost cut logic |
| `check_set_speed_limiter` | Speed limiter logic |
| `calc_iscv` | ISCV duty cycle calculation |
| `calc_speed_kph` | Vehicle speed calculation |
| `check_io_inputs` | Reads and debounces all digital inputs |
| `check_knock_sensor_err_flag` | Knock sensor error detection |
| `clear_nv_ram` | Clears PRAM diagnostic storage |
| `init_ne_on_start` | NE counter initialisation at startup |
| `injector_cold_start` | Cold start enrichment |
| `table_ect_pair_interpolate` | ECT table 2D interpolation |
| `map_rD_rX_interpolate` | 2D map lookup with interpolation |
| `sub_C59B` | Main background process (large subroutine) |
| `sub_EA22` | Secondary background / NE process |

### CPU2 Port Signal Assignments (3S-GTE)

#### Port A ($20h)

| Bit | Signal | Notes |
|---|---|---|
| 4 | VF signal | VF output (0–11), converted externally to analogue voltage |
| 0–3, 5–7 | Unknown | — |

#### Port C ($28h)

| Bit | Signal | Notes |
|---|---|---|
| 7 | Ignition timing retard | When clear, causes ignition timing retard |

#### DOUT ($26h)

| Bit | Signal | Notes |
|---|---|---|
| 0 | DOUT0 output | — |
| 2 | TVSV (boost control solenoid) | Turbo VSV drive |

### Dual-CPU Scheduling (3S-GTE)

Both CPUs use a multi-rate scheduler driven by the 4ms interrupt frame:

| Rate | Mechanism |
|---|---|
| 4ms | Primary interrupt frame; NE (crank) events also trigger at this rate |
| 8ms | flags_40.6 toggled every other 4ms frame (CPU2) |
| 16ms | Triggered by flags_41.4 / 41.5 cleared in 4ms ISR (CPU2) |
| 32ms | Triggered by flags_41.6 (CPU2) |
| 64ms | Triggered by flags_41.7 (CPU2) |

### CPU2 Key Subroutines (3S-GTE, known)

| Label | Description |
|---|---|
| `reset_vector` | Reset / boot entry point |
| `main_loop` | Main background loop |
| `serial_wait_for_data` | Polls SSD for incoming serial data |
| `calc_rpm` | RPM calculation from NE period sum |
| `drive_DOUT0` | Drive DOUT bit 0 |
| `drive_DOUT2_tvsv` | Drive TVSV boost control solenoid |

---

*Appendix derived from Denso 8X (7433) test notes by Jon Hacker and Henri de Rauly, and from IDA disassemblies of Toyota 3S-GTE ECU ROMs D151803-9651 (CPU1) and D151803-9661 (CPU2).*  
*Original instruction set reference from `toshiba-8x-datasheet.pdf` v0.01 by David Sobon (30-Apr-2011).*

