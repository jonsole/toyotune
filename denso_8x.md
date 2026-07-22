# Toshiba 8X MCU — Technical Reference

**Document version:** 0.01  
**Based on:** M68HC11 reference datasheet manual  
**Original source:** `tdis.pl` by Greg Kunyavsky; compiled by David Sobon  
**Date of original:** 30 April 2011

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture Summary](#architecture-summary)
3. [Registers](#registers)
4. [Condition Code Register (CCR) Flags](#condition-code-register-ccr-flags)
5. [Addressing Modes](#addressing-modes)
6. [Instruction Set](#instruction-set)
   - [Add Instructions](#add-instructions)
   - [Branch, Jump, Subroutine & Control Flow](#branch-jump-subroutine--control-flow)
   - [Compare Instructions](#compare-instructions)
   - [Load Instructions](#load-instructions)
   - [Logical Instructions (AND, OR, XOR)](#logical-instructions-and-or-xor)
   - [Increment, Decrement, Clear & Set](#increment-decrement-clear--set)
   - [Subtract & Negate](#subtract--negate)
   - [Store, Push & Pull](#store-push--pull)
   - [Move, Exchange, Multiply & Divide](#move-exchange-multiply--divide)
   - [Rotate & Shift](#rotate--shift)
7. [Instruction OpCode Matrix](#instruction-opcode-matrix)
8. [Register & Control Bit Assignments](#register--control-bit-assignments)
9. [Notes on M68HC11 Compatibility](#notes-on-m68hc11-compatibility)

---

## Overview

The **Toshiba 8X** (D8X) is a proprietary microcontroller unit (MCU) with an instruction set derived from the Motorola M68HC11 architecture. It uses a remapped opcode space and introduces several new instructions not present in the 6811, including hardware multiply/divide, exchange operations, and auto-increment load/store addressing.

This document covers the complete instruction set, opcode matrix, and memory-mapped register assignments.

---

## Architecture Summary

| Feature | Detail |
|---|---|
| Architecture base | Motorola M68HC11 (remapped opcodes) |
| Data registers | A (8-bit), B (8-bit), D = A:B (16-bit) |
| Index registers | X (16-bit), Y (16-bit) |
| Stack pointer | SP (S) |
| Condition Code Register | CCR (H, I, N, Z, V, C flags) |
| Addressing modes | INH, IMM, DIR, EXT, IND,X, IND,Y |

---

## Registers

| Register | Width | Description |
|---|---|---|
| A | 8-bit | Accumulator A |
| B | 8-bit | Accumulator B |
| D | 16-bit | Double accumulator (A = high byte, B = low byte) |
| X | 16-bit | Index register X |
| Y | 16-bit | Index register Y |
| SP (S) | 16-bit | Stack pointer |
| CCR | 8-bit | Condition code register (flags) |

---

## Condition Code Register (CCR) Flags

| Flag | Bit | Description |
|---|---|---|
| H | Half-carry | Set on carry from bit 3 to bit 4 (BCD use) |
| I | Interrupt mask | When set, maskable interrupts are disabled |
| N | Negative | Set when result MSB = 1 |
| Z | Zero | Set when result = 0 |
| V | Overflow | Set on two's complement overflow |
| C | Carry | Set on carry out of MSB |

**Flag notation used in instruction tables:**
- `⧫` — affected by the instruction
- `0` — cleared by the instruction
- `1` — set by the instruction
- `-` — not affected

---

## Addressing Modes

| Mode | Abbreviation | Description |
|---|---|---|
| Inherent | INH | Operand(s) are implicit in the opcode |
| Immediate | IMM | Operand is a literal value (`#xx` or `#xxxx`) |
| Direct | DIR | 8-bit address in zero page (`$xx`) |
| Extended | EXT | Full 16-bit address (`$xxxx`) |
| Indexed X | IND,X | Address = X + 8-bit unsigned offset |
| Indexed Y | IND,Y | Address = Y + 8-bit unsigned offset |
| Relative | REL | Signed 8-bit offset from PC (branch instructions) |

---

## Instruction Set

### Convention

In the tables below:
- **D8X OpCode** — opcode byte on the Toshiba 8X
- **6811 Equiv** — corresponding M68HC11 mnemonic and opcode (where applicable)
- `---` indicates no equivalent or unimplemented on that platform

---

### Add Instructions

| D8X Mnemonic | D8X OpCode | 6811 Mnemonic | 6811 OpCode | Description | Mode | H | I | N | Z | V | C |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `add a, b` | 0x08 | ABA | 0x1B | A ← A + B | INH | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add x, b` | 0x0E | ABX | 0x3A | X ← X + (00:B) | INH | - | - | - | - | - | - |
| `add y, b` | 0x0F | ABY | 0x3A | Y ← Y + (00:B) | INH | - | - | - | - | - | - |
| `add a, #xx` | 0xC0 | ADDA | 0x8B | A ← A + #xx | IMM | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add a, $xx` | 0xD0 | ADDA | 0x9B | A ← A + $xx | DIR | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add a, $xxxx` | 0xF0 | ADDA | 0xBB | A ← A + $xxxx | EXT | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add a, x + 0xXX` | 0xE0 | ADDA | 0xAB | A ← A + $&(X+O) | IND,X | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add a, y + 0xXX` | 0xE0 | ADDA | 0xAB | A ← A + $&(Y+O) | IND,Y | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add b, #xx` | 0xC1 | ADDB | 0xCB | B ← B + #xx | IMM | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add b, $xx` | 0xD1 | ADDB | 0xDB | B ← B + $xx | DIR | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add b, $xxxx` | 0xF1 | ADDB | 0xFB | B ← B + $xxxx | EXT | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add b, x + 0xXX` | 0xE1 | ADDB | 0xEB | B ← B + $&(X+O) | IND,X | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add b, y + 0xXX` | 0xE1 | ADDB | 0xEB | B ← B + $&(Y+O) | IND,Y | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add d, #xxxx` | 0x87 | ADDD | 0xC3 | D ← D + #xxxx | IMM | - | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add d, $xx` | 0x97 | ADDD | 0xD3 | D ← D + $(xx:xx+1) | DIR | - | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add d, $xxxx` | 0xB7 | ADDD | 0xF3 | D ← D + $(xxxx:xxxx+1) | EXT | - | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add d, x + 0xXX` | 0xA7 | ADDD | 0xE3 | D ← D + $&(X+O:X+O+1) | IND,X | - | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `add d, y + 0xXX` | 0xA7 | ADDD | 0xE3 | D ← D + $&(Y+O:Y+O+1) | IND,Y | - | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `addc a, #xx` | 0x80 | ADCA | 0x89 | A ← A + #xx + C | IMM | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `addc a, $xx` | 0x90 | ADCA | 0x99 | A ← A + $xx + C | DIR | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `addc a, $xxxx` | 0xB0 | ADCA | 0xB9 | A ← A + $xxxx + C | EXT | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `addc a, x + 0xXX` | 0xA0 | ADCA | 0xA9 | A ← A + $&(X+O) + C | IND,X | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |
| `addc a, y + 0xXX` | 0xA0 | ADCA | 0xA9 | A ← A + $&(Y+O) + C | IND,Y | ⧫ | - | ⧫ | ⧫ | ⧫ | ⧫ |

> **Note:** ADCB (add-with-carry for B) opcodes present in the M68HC11 have no D8X equivalent.

---

### Branch, Jump, Subroutine & Control Flow

| D8X Mnemonic | D8X OpCode | 6811 Mnemonic | 6811 OpCode | Condition | Mode |
|---|---|---|---|---|---|
| `bra 0xXX` | 0x40 | BRA | 0x20 | Always | REL |
| `brn 0xXX` | 0x41 | BRN | 0x21 | Never | REL |
| `bgt 0xXX` | 0x42 | BHI | 0x22 | C + Z = 0 | REL |
| `ble 0xXX` | 0x43 | BLS | 0x23 | C + Z = 1 | REL |
| `bcc 0xXX` | 0x44 | BCC/BHS | 0x24 | C = 0 | REL |
| `bcs 0xXX` | 0x45 | BCS/BLO | 0x25 | C = 1 | REL |
| `bne 0xXX` | 0x46 | BNE | 0x26 | Z = 0 | REL |
| `beq 0xXX` | 0x47 | BEQ | 0x27 | Z = 1 | REL |
| `bvc 0xXX` | 0x48 | BVC | 0x28 | V = 0 | REL |
| `bvs 0xXX` | 0x49 | BVS | 0x29 | V = 1 | REL |
| `bpz 0xXX` | 0x4A | BPL | 0x2A | N = 0 | REL |
| `bmi 0xXX` | 0x4B | BMI | 0x2B | N = 1 | REL |
| `bge 0xXX` | 0x4C | BGE | 0x2C | N ^ V = 0 | REL |
| `blta 0xXX` | 0x4D | BLT | 0x2D | N ^ V = 1 | REL |
| `bgta 0xXX` | 0x4E | BGT | 0x2E | Z + (N^V) = 0 | REL |
| `blea 0xXX` | 0x4F | BLE | 0x2F | Z + (N^V) = 1 | REL |
| `tbbc bit.#, $xx, 0x..` | 0x37 | BRCLR | 0x13 | $xx.bit# = 0 → branch | DIR |
| `tbbs bit.#, $xx, 0x..` | 0x35 | BRSET | 0x12 | $xx.bit# = 1 → branch | DIR |
| `bsr 0xXX` | 0x61 | BSR | 0x8D | Jump to PC+offset, push return | REL |
| `jmp 0xXXXX` | 0x03 | JMP | 0x7E | Jump absolute | EXT |
| `jmp x + 0xXX` | 0x23 | JMP | 0x6E | Jump X+offset | IND,X |
| `jmp y + 0xXX` | 0x23 | JMP | 0x6E | Jump Y+offset | IND,Y |
| `jsr $xx` | 0x31 | JSR | 0x9D | Call subroutine (dir) | DIR |
| `jsr 0xXXXX` | 0x01 | JSR | 0xBD | Call subroutine (ext) | EXT |
| `jsr x + 0xXX` | 0x21 | JSR | 0xAD | Call subroutine (ind X) | IND,X |
| `jsr y + 0xXX` | 0x21 | JSR | 0xAD | Call subroutine (ind Y) | IND,Y |
| `nop` | 0x00 | NOP | 0x01 | No operation | INH |
| `reti` | 0x73 | RTI | 0x3B | Return from interrupt (restores CCR) | INH |
| `ret` | 0x63 | RTS | 0x39 | Return from subroutine | INH |
| `wait` | 0x83 | WAI | 0x3E | Stack registers and wait for interrupt | INH |

---

### Compare Instructions

| D8X Mnemonic | D8X OpCode | Description | Mode | N | Z | V | C |
|---|---|---|---|---|---|---|---|
| `cmp a, b` | 0x0B | TEST(A − B) | INH | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmpz a` | 0x58 | TEST(A − 0) | INH | ⧫ | ⧫ | 0 | 0 |
| `cmpz b` | 0x59 | TEST(B − 0) | INH | ⧫ | ⧫ | 0 | 0 |
| `cmp a, #xx` | 0xCC | TEST(A − #xx) | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp a, $xx` | 0xDC | TEST(A − $xx) | DIR | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp a, $xxxx` | 0xFC | TEST(A − $xxxx) | EXT | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp a, x + 0xXX` | 0xEC | TEST(A − $&(X+O)) | IND,X | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp b, #xx` | 0xCD | TEST(B − #xx) | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp b, $xx` | 0xDD | TEST(B − $xx) | DIR | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp b, $xxxx` | 0xFD | TEST(B − $xxxx) | EXT | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp x, #xxxx` | 0x8C | TEST(X − #xxxx) | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp x, $xx` | 0x9C | TEST(X − $(xx:xx+1)) | DIR | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp x, $xxxx` | 0xBC | TEST(X − $(xxxx:xxxx+1)) | EXT | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp y, #xxxx` | 0x8D | TEST(Y − #xxxx) | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmp d, #xxxx` | 0x89 | TEST(D − #xxxx) | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `cmpb a, #xx` | 0xCD | TEST(A & #xx) — bit test | IMM | ⧫ | ⧫ | 0 | - |
| `cmpb a, $xx` | 0xDD | TEST(A & $xx) | DIR | ⧫ | ⧫ | 0 | - |
| `cmpb b, #xx` | 0xCF | TEST(B & #xx) | IMM | ⧫ | ⧫ | 0 | - |

> **Note:** `cmpb` performs a bit-wise AND test (equivalent to 6811 BITA/BITB). Result is discarded; only flags are updated. V is always cleared.

---

### Load Instructions

| D8X Mnemonic | D8X OpCode | Description | Mode | N | Z | V |
|---|---|---|---|---|---|---|
| `ld a, [y]` | 0x1A | A ← $&(Y); Y++ | INH | - | - | - |
| `ld d, [y]` | 0x1B | D ← $&(Y:Y+1); Y+=2 | INH | - | - | - |
| `ld a, #xx` | 0xCA | A ← #xx | IMM | ⧫ | ⧫ | 0 |
| `ld a, $xx` | 0xDA | A ← $xx | DIR | ⧫ | ⧫ | 0 |
| `ld a, 0xXXXX` | 0xFA | A ← $xxxx | EXT | ⧫ | ⧫ | 0 |
| `ld a, x + 0xXX` | 0xEA | A ← $&(X+offset) | IND,X | ⧫ | ⧫ | 0 |
| `ld a, y + 0xXX` | 0xEA | A ← $&(Y+offset) | IND,Y | ⧫ | ⧫ | 0 |
| `ld b, #xx` | 0xCB | B ← #xx | IMM | ⧫ | ⧫ | 0 |
| `ld b, $xx` | 0xDB | B ← $xx | DIR | ⧫ | ⧫ | 0 |
| `ld b, 0xXXXX` | 0xFB | B ← $xxxx | EXT | ⧫ | ⧫ | 0 |
| `ld d, #xxxx` | 0x86 | D ← #xxxx | IMM | ⧫ | ⧫ | 0 |
| `ld d, $xx` | 0x96 | D ← $(xx:xx+1) | DIR | ⧫ | ⧫ | 0 |
| `ld d, 0xXXXX` | 0xB6 | D ← $(xxxx:xxxx+1) | EXT | ⧫ | ⧫ | 0 |
| `ld s, #xxxx` | 0x8B | SP ← #xxxx | IMM | ⧫ | ⧫ | 0 |
| `ld x, #xxxx` | 0x8E | X ← #xxxx | IMM | ⧫ | ⧫ | 0 |
| `ld y, #xxxx` | 0x8F | Y ← #xxxx | IMM | ⧫ | ⧫ | 0 |

> **Note:** The `ld a, [y]` and `ld d, [y]` instructions use auto-increment addressing — Y is incremented after each load. These are D8X-specific instructions with no M68HC11 equivalent.

---

### Logical Instructions (AND, OR, XOR)

All logical instructions clear V; C is unaffected. N and Z reflect the result.

#### AND

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `and a, #xx` | 0xC2 | A ← A & #xx | IMM |
| `and a, $xx` | 0xD2 | A ← A & $xx | DIR |
| `and a, $xxxx` | 0xF2 | A ← A & $xxxx | EXT |
| `and a, x + 0xXX` | 0xE2 | A ← A & $&(X+O) | IND,X |
| `and b, #xx` | 0xC3 | B ← B & #xx | IMM |
| `and b, $xx` | 0xD3 | B ← B & $xx | DIR |
| `and b, $xxxx` | 0xF3 | B ← B & $xxxx | EXT |

#### OR

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `or a, #xx` | 0xC6 | A ← A \| #xx | IMM |
| `or a, $xx` | 0xD6 | A ← A \| $xx | DIR |
| `or a, $xxxx` | 0xF6 | A ← A \| $xxxx | EXT |
| `or b, #xx` | 0xC7 | B ← B \| #xx | IMM |
| `or b, $xx` | 0xD7 | B ← B \| $xx | DIR |

#### XOR

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `xor a, #xx` | 0xC8 | A ← A ^ #xx | IMM |
| `xor a, $xx` | 0xD8 | A ← A ^ $xx | DIR |
| `xor a, $xxxx` | 0xF8 | A ← A ^ $xxxx | EXT |
| `xor b, #xx` | 0xC9 | B ← B ^ #xx | IMM |
| `xor b, $xx` | 0xD9 | B ← B ^ $xx | DIR |

---

### Increment, Decrement, Clear & Set

#### Increment

| D8X Mnemonic | D8X OpCode | Description | Mode | N | Z | V |
|---|---|---|---|---|---|---|
| `inc $xx` | 0x76 | $xx ← $xx + 1 | EXT | ⧫ | ⧫ | ⧫ |
| `inc x + 0xXX` | 0x66 | $&(X+O)++ | IND,X | ⧫ | ⧫ | ⧫ |
| `inc a` | 0x56 | A ← A + 1 | INH | ⧫ | ⧫ | ⧫ |
| `inc b` | 0x57 | B ← B + 1 | INH | ⧫ | ⧫ | ⧫ |
| `inc x` | 0x1C | X ← X + 1 | INH | - | ⧫ | - |
| `inc y` | 0x1D | Y ← Y + 1 | INH | - | ⧫ | - |
| `inc s` | 0x2D | SP ← SP + 1 | INH | - | - | - |

#### Decrement

| D8X Mnemonic | D8X OpCode | Description | Mode | N | Z | V |
|---|---|---|---|---|---|---|
| `dec $xx` | 0x70 | $xx ← $xx − 1 | EXT | ⧫ | ⧫ | ⧫ |
| `dec a` | 0x50 | A ← A − 1 | INH | ⧫ | ⧫ | ⧫ |
| `dec b` | 0x51 | B ← B − 1 | INH | ⧫ | ⧫ | ⧫ |
| `dec x` | 0x1E | X ← X − 1 | INH | - | ⧫ | - |
| `dec y` | 0x1F | Y ← Y − 1 | INH | - | ⧫ | - |
| `dec s` | 0x2F | SP ← SP − 1 | INH | - | - | - |

#### Clear

| D8X Mnemonic | D8X OpCode | Description | Mode | N | Z | V | C |
|---|---|---|---|---|---|---|---|
| `clr $xx` | 0x72 | $xx ← 0 | EXT | 0 | 1 | 0 | 0 |
| `clr x + 0xXX` | 0x62 | $&(X+O) ← 0 | IND,X | 0 | 1 | 0 | 0 |
| `clr a` | 0x52 | A ← 0 | INH | 0 | 1 | 0 | 0 |
| `clr b` | 0x53 | B ← 0 | INH | 0 | 1 | 0 | 0 |
| `clrb bit#, $xx` | 0x75 | $xx.bit# ← 0 | DIR | ⧫ | ⧫ | 0 | - |
| `clrc` | 0x65 | C ← 0 | INH | - | - | - | 0 |
| `clrv` | 0x25 | V ← 0 | INH | - | - | 0 | - |

#### Set

| D8X Mnemonic | D8X OpCode | Description | Mode | V/C |
|---|---|---|---|---|
| `setb bit#, $xx` | 0x77 | $xx.bit# ← 1 | DIR | - |
| `setc` | 0x67 | C ← 1 | INH | C=1 |
| `setv` | 0x27 | V ← 1 | INH | V=1 |
| `di` | 0x05 | I ← 0 (interrupts enabled) | INH | I=0 |
| `ei` | 0x07 | I ← 1 (interrupts disabled) | INH | I=1 |

---

### Subtract & Negate

#### Subtract

| D8X Mnemonic | D8X OpCode | Description | Mode | N | Z | V | C |
|---|---|---|---|---|---|---|---|
| `sub a, b` | 0x09 | A ← A − B | INH | ⧫ | ⧫ | ⧫ | ⧫ |
| `sub a, #xx` | 0xC4 | A ← A − #xx | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `sub a, $xx` | 0xD4 | A ← A − $xx | DIR | ⧫ | ⧫ | ⧫ | ⧫ |
| `sub b, #xx` | 0xC5 | B ← B − #xx | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `sub d, #xxxx` | 0x88 | D ← D − #xxxx | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `subc a, #xx` | 0x84 | A ← A − #xx − C | IMM | ⧫ | ⧫ | ⧫ | ⧫ |
| `subc a, $xx` | 0x94 | A ← A − $xx − C | DIR | ⧫ | ⧫ | ⧫ | ⧫ |

> **Note:** SBCB (subtract with carry for B) has no D8X equivalent.

#### Negate

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `neg $xx` | 0x74 | $xx ← 0 − $xx | EXT |
| `neg x + 0xXX` | 0x64 | $&(X+O) ← 0 − $&(X+O) | IND,X |
| `neg a` | 0x54 | A ← 0 − A | INH |
| `neg b` | 0x55 | B ← 0 − B | INH |

#### BCD Adjust

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `adj a` | 0x5E | Decimal-adjust A after BCD addition (DAA) | INH |

---

### Store, Push & Pull

#### Store

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `st a, [y]` | 0x82 | $&(Y) ← A; Y++ | INH |
| `st d, [y]` | 0x8A | $&(Y:Y+1) ← D; Y+=2 | INH |
| `st a, $xx` | 0x92 | $xx ← A | DIR |
| `st a, $xxxx` | 0xB2 | $xxxx ← A | EXT |
| `st a, x + 0xXX` | 0xA2 | $&(X+O) ← A | IND,X |
| `st b, $xx` | 0x93 | $xx ← B | DIR |
| `st b, $xxxx` | 0xB3 | $xxxx ← B | EXT |
| `st d, $xx` | 0x9A | $(xx:xx+1) ← D | DIR |
| `st d, $xxxx` | 0xBA | $(xxxx:xxxx+1) ← D | EXT |
| `st s, $xx` | 0x39 | $(xx:xx+1) ← SP | DIR |
| `st x, $xx` | 0x0A | $(xx:xx+1) ← X | DIR |
| `st x, $xxxx` | 0x3A | $(xxxx:xxxx+1) ← X | EXT |
| `st y, $xx` | 0x3B | $(xx:xx+1) ← Y | DIR |

#### Push (stack)

| D8X Mnemonic | D8X OpCode | Description |
|---|---|---|
| `push a` | 0x6C | STACK ← A; SP ← SP − 1 |
| `push b` | 0x6D | STACK ← B; SP ← SP − 1 |
| `push x` | 0x6E | STACK ← X; SP ← SP − 2 |
| `push y` | 0x6F | STACK ← Y; SP ← SP − 2 |

#### Pull (stack)

| D8X Mnemonic | D8X OpCode | Description |
|---|---|---|
| `pull a` | 0x7C | SP ← SP + 1; A ← STACK |
| `pull b` | 0x7D | SP ← SP + 1; B ← STACK |
| `pull x` | 0x7E | SP ← SP + 2; X ← STACK |
| `pull y` | 0x7F | SP ← SP + 2; Y ← STACK |

---

### Move, Exchange, Multiply & Divide

#### Move (register transfers)

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `mov b, a` | 0x5A | A ← B | INH |
| `mov a, b` | 0x5B | B ← A | INH |
| `mov x, d` | 0x3C | D ← X | INH |
| `mov y, d` | 0x3D | D ← Y | INH |
| `mov d, x` | 0x3E | X ← D | INH |
| `mov d, y` | 0x3F | Y ← D | INH |
| `mov s, x` | 0x2E | X ← SP + 1 | INH |
| `mov x, s` | 0x2C | SP ← X − 1 | INH |
| `mov a, ocr` | 0x5D | CCR ← A | INH |
| `mov ocr, a` | 0x5C | A ← CCR | INH |

#### Exchange

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `xch a, b` | 0x02 | A ↔ B | INH |
| `xch x, y` | 0x69 | X ↔ Y | INH |
| `xch a, $xx` | 0x7A | A ↔ $xx | DIR |
| `xch a, x + 0xXX` | 0x6A | A ↔ $&(X+O) | IND,X |
| `xch b, $xx` | 0x7B | B ↔ $xx | DIR |
| `xch b, x + 0xXX` | 0x6B | B ↔ $&(X+O) | IND,X |

#### Multiply (D8X-specific)

| D8X Mnemonic | D8X OpCode | Description | Mode | C |
|---|---|---|---|---|
| `mul a, #xx` | 0x81 | D ← A × #xx | IMM | ⧫ |
| `mul a, $xx` | 0x91 | D ← A × $xx | DIR | ⧫ |
| `mul a, $xxxx` | 0xB1 | D ← A × $xxxx | EXT | ⧫ |
| `mul a, x + 0xXX` | 0xA1 | D ← A × $&(X+O) | IND,X | ⧫ |

> Result is 16-bit stored in D. Only C flag is affected.

#### Divide (D8X-specific)

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `div d, #xx` | 0x85 | B ← D ÷ #xx; A ← D mod #xx | IMM |
| `div d, $xx` | 0x95 | B ← D ÷ $xx; A ← D mod $xx | DIR |
| `div d, $xxxx` | 0xB5 | B ← D ÷ $xxxx; A ← D mod $xxxx | EXT |
| `div d, x + 0xXX` | 0xA5 | B ← D ÷ $&(X+O); A ← remainder | IND,X |

> Quotient in B, remainder in A.

---

### Rotate & Shift

#### Rotate Left through Carry (ROLC)

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `rolc $xx` | 0x36 | $xx ← $xx << 1 (←C←) | DIR |
| `rolc x + 0xXX` | 0x26 | $&(X+O) ← $&(X+O) << 1 | IND,X |
| `rolc a` | 0x16 | A ← A << 1 (←C←) | INH |
| `rolc b` | 0x17 | B ← B << 1 (←C←) | INH |

#### Rotate Right through Carry (RORC)

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `rorc $xx` | 0x34 | $xx ← $xx >> 1 (→C→) | DIR |
| `rorc a` | 0x14 | A ← A >> 1 (→C→) | INH |
| `rorc b` | 0x15 | B ← B >> 1 (→C→) | INH |

#### Arithmetic Shift Right (SHRA)

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `shra $xx` | 0x38 | $xx ← $xx >> 1 (sign extended) | EXT |
| `shra a` | 0x18 | A ← A >> 1 (sign extended) | INH |
| `shra b` | 0x19 | B ← B >> 1 (sign extended) | INH |

#### Logical Shift Left (SHL)

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `shl $xx` | 0x32 | $xx ← $xx << 1 | DIR |
| `shl a` | 0x12 | A ← A << 1 | INH |
| `shl b` | 0x13 | B ← B << 1 | INH |
| `shl d` | 0x06 | D ← D << 1 | INH |
| `shl x` | 0x22 | X ← X << 1 | INH |

#### Logical Shift Right (SHR)

| D8X Mnemonic | D8X OpCode | Description | Mode |
|---|---|---|---|
| `shr $xx` | 0x30 | $xx ← $xx >> 1 (0 into MSB) | DIR |
| `shr a` | 0x10 | A ← A >> 1 | INH |
| `shr b` | 0x11 | B ← B >> 1 | INH |
| `shr d` | 0x04 | D ← D >> 1 | INH |
| `shr x` | 0x20 | X ← X >> 1 | INH |

---

## Instruction OpCode Matrix

The full 16×16 opcode matrix (row = upper nibble, column = lower nibble):

|  | x0 | x1 | x2 | x3 | x4 | x5 | x6 | x7 | x8 | x9 | xA | xB | xC | xD | xE | xF |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **0x** | nop | jsr & | xch ab | jmp | shr d | di | shl d | ei | add ba | sub ba | st x,$ | cmp ba | add ax | add ay | add bx | add by |
| **1x** | shr a | shr b | shl a | shl b | rorc a | rorc b | rolc a | rolc b | shra a | shra b | ld a,[y] | ld d,[y] | inc x | inc y | dec x | dec y |
| **2x** | shr x | jsr &+ | shl x | jmp &+ | rorc &+ | clrv | rolc &+ | setv | shra &+ | st s,&+ | st x,&+ | st y,&+ | mov xs | inc s | mov sx | dec s |
| **3x** | shr $ | jsr $ | shl $ | ld #$ | rorc $ | tbbs | rolc $ | tbbc | shra $ | st s,$ | st x,$ | st y,$ | mov xd | mov yd | mov dx | mov dy |
| **4x** | bra | brn | bgt | ble | bcc | bcs | bne | beq | bvc | bvs | bpz | bmi | bge | blta | bgta | blea |
| **5x** | dec a | dec b | clr a | clr b | neg a | neg b | inc a | inc b | cmpz a | cmpz b | mov ba | mov ab | mov Ca | mov aC | adj a | nmi |
| **6x** | dec &+ | bsr | clr &+ | ret | neg &+ | clr c | inc &+ | set c | push d | xch xy | xch a | xch b | push a | push b | push x | push y |
| **7x** | dec $ | tbs | clr $ | reti | neg $ | clrb $ | inc $ | setb $ | pull d | cmp #$ | xch a,$ | xch b,$ | pull a | pull b | pull x | pull y |
| **8x** | addca | mul a | st a,[y] | wait | subc a | div d | ld d | add d | sub d | cmp d | st d,[y] | ld s | cmp x | cmp y | ld x | ld y |
| **9x** | addca | mul a | st a | st b | subc a | div d | ld d | add d | sub d | cmp d | st d | ld s | cmp x | cmp y | ld x | ld y |
| **Ax** | addca | mul a | st a | st b | subc a | div d | ld d | add d | sub d | cmp d | st d | ld s | cmp x | cmp y | ld x | ld y |
| **Bx** | addca | mul a | st a | st b | subc a | div d | ld d | add d | sub d | cmp d | st d | ld s | cmp x | cmp y | ld x | ld y |
| **Cx** | add a | add b | and a | and b | sub a | sub b | or a | or b | xor a | xor b | ld a | ld b | cmp a | cmp b | cmpb a | cmpb b |
| **Dx** | add a | add b | and a | and b | sub a | sub b | or a | or b | xor a | xor b | ld a | ld b | cmp a | cmp b | cmpb a | cmpb b |
| **Ex** | add a | add b | and a | and b | sub a | sub b | or a | or b | xor a | xor b | ld a | ld b | cmp a | cmp b | cmpb a | cmpb b |
| **Fx** | add a | add b | and a | and b | sub a | sub b | or a | or b | xor a | xor b | ld a | ld b | cmp a | cmp b | cmpb a | cmpb b |

> Addressing mode is determined by the row (upper nibble): 8x = IMM, 9x = DIR, Ax = IND, Bx = EXT, Cx = IMM, Dx = DIR, Ex = IND, Fx = EXT.

---

## Register & Control Bit Assignments

### $0000–$001F

| Address | Register Name | Notes |
|---|---|---|
| $0000 | DDRA | Data direction register A |
| $0001 | DDRB | Data direction register B |
| $0002 | WDC | Watchdog control |
| $0003 | TIMER3 | Timer 3 |
| $0004 | TIMER | Timer (main) |
| $0005 | TIMERL | Timer low byte |
| $0006 | SIDR_SODR | Serial input/output data register |
| $0007 | SMRC_SIR | Serial mode/control / serial input register |
| $0008 | CPR0 | Compare register 0 (high) |
| $0009 | CPR0L | Compare register 0 (low) |
| $000A | CPR1 | Compare register 1 (high) |
| $000B | CPR1L | Compare register 1 (low) |
| $000C | CPR2 | Compare register 2 (high) |
| $000D | CPR2L | Compare register 2 (low) |
| $000E | CPR3 | Compare register 3 (high) |
| $000F | CPR3L | Compare register 3 (low) |
| $0010 | ASR0P | Analogue sample register 0 positive (high) |
| $0011 | ASR0PL | Analogue sample register 0 positive (low) |
| $0012 | ASR0N | Analogue sample register 0 negative (high) |
| $0013 | ASR0NL | Analogue sample register 0 negative (low) |
| $0014 | ASR1P | Analogue sample register 1 positive (high) |
| $0015 | ASR1PL | Analogue sample register 1 positive (low) |
| $0016 | ASR1N | Analogue sample register 1 negative (high) |
| $0017 | ASR1NL | Analogue sample register 1 negative (low) |
| $0018 | ASR2 | Analogue sample register 2 (high) |
| $0019 | ASR2L | Analogue sample register 2 (low) |
| $001A | ASR3 | Analogue sample register 3 (high) |
| $001B | ASR3L | Analogue sample register 3 (low) |
| $001C–$001E | — | Reserved |
| $001F | OMODE | Operating mode register |

### $0020–$003F

| Address | Register Name | Notes |
|---|---|---|
| $0020 | PORTA | Port A data register |
| $0021 | PORTAL | Port A latch |
| $0022 | PORTB | Port B data register |
| $0023 | PBCS | Port B chip select |
| $0024 | TAIT | Timer A interrupt trigger |
| $0025 | LDOUT | Latched output |
| $0026 | DOUT | Direct output |
| $0027 | DOM | Data output mode |
| $0028 | PORTC | Port C data register |
| $0029 | PORTD_ASRIN | Port D / analogue sample input |
| $002A | RAMST | RAM status |
| $002B | SSD | Serial shift data |
| $002C | IRQL | IRQ latch (high) |
| $002D | IRQLL | IRQ latch (low) |
| $002E | IMASK | Interrupt mask (high) |
| $002F | IMASKL | Interrupt mask (low) |
| $0030–$0037 | — | Reserved |
| $0038 | CPR4 | Compare register 4 (high) |
| $0039 | CPR4L | Compare register 4 (low) |
| $003A | CPR5 | Compare register 5 (high) |
| $003B | CPR5L | Compare register 5 (low) |
| $003C | CPR6 | Compare register 6 (high) |
| $003D | CPR6L | Compare register 6 (low) |
| $003E | CPR7 | Compare register 7 (high) |
| $003F | CPR7L | Compare register 7 (low) |

---

## Notes on M68HC11 Compatibility

The Toshiba D8X is **not** binary-compatible with the M68HC11. Key differences:

- **Opcode remapping:** Nearly all opcodes are different. The D8X uses a single-byte opcode space (no pre-byte escape sequences), whereas the 6811 uses `$18` and `$1A` prefix bytes for certain Y-register instructions.
- **New instructions:** The D8X adds `mul`, `div`, `xch`, `ld a,[y]` (auto-increment), `st a,[y]` (auto-increment), and extended shift instructions (`shl x`, `shr x`) not present in the 6811.
- **Missing instructions:** Several 6811 instructions have no D8X equivalent, including `ADCB`, `SBCB`, `COM`, `FDIV`, `IDIV`, `XGDX`, `XGDY`, `TSY`, `TYS`, `STOP`, `SWI`, and `TEST`.
- **Mnemonic differences:** D8X uses a simplified, uniform mnemonic style (e.g., `ld`, `st`, `cmp`, `add`) rather than the register-suffixed 6811 style (e.g., `LDAA`, `STAB`, `CMPA`).
- **Branch naming:** Several branch conditions are renamed for clarity (e.g., `bpz` instead of `BPL`, `blta`/`bgta` instead of `BLT`/`BGT`).

---

*This document was generated from `toshiba-8x-datasheet.pdf` (v0.01, 30-Apr-2011).*

---

# Denso 8X (7433) — Test Notes & Hardware Reference

**Sources:** Jon Hacker, Henri de Rauly  
**ECU application:** Toyota 7M-GE (Supra / Cressida era)

---

## Table of Contents (Test Notes)

1. [Package & Pinouts](#package--pinouts)
2. [Memory Map](#memory-map)
3. [I/O Register Reference (Expanded)](#io-register-reference-expanded)
4. [Interrupt System](#interrupt-system)
   - [Interrupt Overview](#interrupt-overview)
   - [IMASK / IRQL Registers](#imask--irql-registers)
   - [Stack Handling on IRQ](#stack-handling-on-irq)
   - [Interrupt Vector Analysis](#interrupt-vector-analysis)
5. [Timer System](#timer-system)
6. [ASR (Analogue Signal / Edge Counter) Subsystem](#asr-analogue-signal--edge-counter-subsystem)
7. [DOUT / LDOUT / DOM](#dout--ldout--dom)
8. [Port A / Port B / Port C / Port D](#port-a--port-b--port-c--port-d)
9. [WI Pin and PRAM (Preserved RAM)](#wi-pin-and-pram-preserved-ram)
10. [Serial Communications](#serial-communications)
11. [Watch Dog Timer (WDC)](#watch-dog-timer-wdc)
12. [MIL Light Timing](#mil-light-timing)
13. [Branch Operations — Validated Behaviour](#branch-operations--validated-behaviour)
14. [Bit Operations — Address Mapping](#bit-operations--address-mapping)
15. [Division Instruction — Validated Behaviour](#division-instruction--validated-behaviour)
16. [7M-GE Knock MCU Interface](#7m-ge-knock-mcu-interface)

---

## Package & Pinouts

The D8X MCU is available in two packages observed in this ECU:

### SDIP64 (Main MCU)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | ASR1 | 64 | VCC |
| 2 | ASR0 | 63 | ASR2 |
| 3 | NMI̅ | 62 | ASR3 |
| 4 | IRP | 61 | DOUT0 |
| 5 | IRL | 60 | DOUT1 |
| 6 | XIN | 59 | DOUT2 |
| 7 | XOUT | 58 | DOUT3 |
| 8 | CCLK | 57 | DOUT4 |
| 9 | ADR | 56 | DOUT5 |
| 10 | IS̅/R̅D̅ | 55 | DOUT6 |
| 11 | OS̅/W̅D̅ | 54 | DOUT7 |
| 12 | I/E̅ | 53 | W̅D̅C̅ |
| 13 | CLK | 52 | I̅N̅I̅T̅ |
| 14 | SIN3 | 51 | H̅A̅L̅T̅ |
| 15 | SIN2 | 50 | W̅I̅ |
| 16 | SIN1 | 49 | T̅E̅S̅T̅ |
| 17 | SIN0 | 48 | PC0 |
| 18 | SOUT1 | 47 | PC1 |
| 19 | SOUT0 | 46 | PC2 |
| 20 | PB7/DA7 | 45 | PC3 |
| 21 | PB6/DA6 | 44 | PC4 |
| 22 | PB5/DA5 | 43 | PC5 |
| 23 | PB4/DA4 | 42 | PC6 |
| 24 | PB3/DA3 | 41 | PC7 |
| 25 | PB2/DA2 | 40 | PD0 |
| 26 | PB1/DA1 | 39 | PD1 |
| 27 | PB0/DA0 | 38 | PD2 |
| 28 | PA7/A15 | 37 | PD3 |
| 29 | PA6/A14 | 36 | PA0/A8 |
| 30 | PA5/A13 | 35 | PA1/A9 |
| 31 | PA4/A12 | 34 | PA2/A10 |
| 32 | GND | 33 | PA3/A11 |

> **Note:** A mirrored (back-of-board) pinout is also documented. Pin numbering reverses left–right when viewing from the back. See Chart 3 in source document.

### DIP42 (Secondary / Knock MCU)

| Pin | Signal | Pin | Signal |
|---|---|---|---|
| 1 | W̅I̅ | 42 | VCC |
| 2 | H̅A̅L̅T̅ | 41 | ? |
| 3 | I̅N̅I̅T̅ | 40 | A8 |
| 4–10 | ? (unknown) | 39 | A9 |
| 11 | IRP | 38 | A10 |
| 12 | ? | 37 | A11 |
| 13 | X1 | 36 | A12 |
| 14 | X2 | 35 | A13 |
| 15 | ADR | 34 | A14 |
| 16 | R̅D̅ | 33 | A15 |
| 17 | W̅E̅ | 32 | D0 |
| 18 | I/E̅ | 31 | D1 |
| 19–20 | ? | 30 | D2 |
| 21 | GND | 29 | D3 |
| 22 | ? | 28 | D4 |
| — | — | 27 | D5 |
| — | — | 26 | D6 |
| — | — | 25 | D7 |
| — | — | 24–22 | ? |

---

## Memory Map

Two operating modes documented (from Kashi's notes):

| Region | Mode 2 | Mode 7 |
|---|---|---|
| $0000–$002F | I/O Area | I/O Area |
| $0030–$003F | (gap) | (gap) |
| $0040–$01BF | Built-in RAM | Built-in RAM |
| $01C0–$CFFF | External memory | Unused |
| $D000–$FFDD | — | Built-in ROM (12 kB = 0x3000 bytes) |
| $D000–$FFDD | External memory (up to 63.75 kB) | — |
| $FFDE–$FFFF | Vector area | Vector area |

**Registers in CPU:**

| Register | Description |
|---|---|
| PC | Program Counter |
| CCR | Condition Code Register |
| SP | Stack Pointer |
| Y | Index Register Y |
| X | Index Register X |
| D | Double Accumulator (A:B) |
| B | Accumulator B |
| A | Accumulator A |

**PRAM (Preserved RAM):** Addresses $80h–$9Fh are battery-backed. Controlled by the WI̅ pin and RAMST register (see [WI Pin and PRAM](#wi-pin-and-pram-preserved-ram)).

---

## I/O Register Reference (Expanded)

The following table consolidates register names with functional descriptions derived from test code analysis.

| Address | Name | Function |
|---|---|---|
| $00 | DDRA | Port A I/O direction (0=input, 1=output) |
| $01 | DDRB | Port B I/O direction |
| $02 | WDC | Watchdog timer control (ECU sets 0x5A = `01011010b`) |
| $03 | TIMER3 | Timer LSB bits [2:0] |
| $04 | TIMER | Timer MSB bits [18:11] (top 8 of 19-bit counter) |
| $05 | TIMERL | Timer LSB bits [10:3] |
| $06 | SIDR / SODR | Serial Input / Output Data Register |
| $07 | SMRC / SIR | Serial Master Register Control / Serial Input Register |
| $08 | CPR0 | Timer Compare Register 0 — MSB |
| $09 | CPR0L | Timer Compare Register 0 — LSB |
| $0A | CPR1 | Timer Compare Register 1 — MSB |
| $0B | CPR1L | Timer Compare Register 1 — LSB |
| $0C | CPR2 | Timer Compare Register 2 — MSB |
| $0D | CPR2L | Timer Compare Register 2 — LSB |
| $0E | CPR3 | Timer Compare Register 3 — MSB |
| $0F | CPR3L | Timer Compare Register 3 — LSB |
| $10 | ASR0P | ASR0 rising edge counter — MSB |
| $11 | ASR0PL | ASR0 rising edge counter — LSB |
| $12 | ASR0N | ASR0 falling edge counter — MSB |
| $13 | ASR0NL | ASR0 falling edge counter — LSB |
| $14 | ASR1P | ASR1 rising edge counter — MSB |
| $15 | ASR1PL | ASR1 rising edge counter — LSB |
| $16 | ASR1N | ASR1 falling edge counter — MSB |
| $17 | ASR1NL | ASR1 falling edge counter — LSB |
| $18 | ASR2 | ASR2 edge counter — MSB |
| $19 | ASR2L | ASR2 edge counter — LSB |
| $1A | ASR3 | ASR3 edge counter — MSB |
| $1B | ASR3L | ASR3 edge counter — LSB |
| $1C–$1E | — | Unused / reserved |
| $1F | OMODE | Operating mode select |
| $20 | PORTA | Port A data |
| $21 | PORTAL | Port A latch (latched on rising edge bits 4–5, falling edge bits 6–7; bits 0–3 always 1) |
| $22 | PORTB | Port B data |
| $23 | PBCS | Port B control / status (PBCS.4 = IS̅ pin status; PBCS.5 = IRP pin status; bits 6–7 always 1) |
| $24 | TAIT | Timer / ASR control (see TAIT bit table below) |
| $25 | LDOUT | Latched DOUT data — read only, reflects actual pin state |
| $26 | DOUT | DOUT output data register |
| $27 | DOM | DOUT output mode (0=immediate latch, 1=CPRx-triggered latch) |
| $28 | PORTC | Port C data — input only |
| $29 | PORTD_ASRIN | Port D output (bits 0–3 write) / ASR pin status (bits 4–7 read) |
| $2A | RAMST | Built-in RAM status (bit 5 controls R/W of $80–$9F; bit 7 triggers PRAM purge on RESET) |
| $2B | SSD | Serial Status Data register |
| $2C | IRQL | Interrupt Request Latch — MSB (IRQLH) |
| $2D | IRQLL | Interrupt Request Latch — LSB |
| $2E | IMASK | Interrupt Mask — MSB |
| $2F | IMASKL | Interrupt Mask — LSB |
| $38 | CPR4 | Timer Compare Register 4 — MSB |
| $39 | CPR4L | Timer Compare Register 4 — LSB |
| $3A | CPR5 | Timer Compare Register 5 — MSB |
| $3B | CPR5L | Timer Compare Register 5 — LSB |
| $3C | CPR6 | Timer Compare Register 6 — MSB |
| $3D | CPR6L | Timer Compare Register 6 — LSB |
| $3E | CPR7 | Timer Compare Register 7 — MSB |
| $3F | CPR7L | Timer Compare Register 7 — LSB |

### TAIT Register Bit Assignments ($24h)

| Bit | ECU value | Signal | Description |
|---|---|---|---|
| 0 | 1 | TS0 | Timer scaling bit 0 |
| 1 | 1 | TS1 | Timer scaling bit 1 |
| 2 | 0 | TS2 | Timer scaling bit 2 |
| 3 | 0 | TS3 | Timer scaling bit 3 |
| 4 | 1 | ASR2 edge | 1 = rising edge, 0 = falling edge |
| 5 | 1 | ASR3 edge | 1 = rising edge, 0 = falling edge |
| 6 | 0 | — | Unknown |
| 7 | 0 | — | Unknown |

> TAIT bits 0–3 (TS0–TS3) control the TIMER prescaler, which affects BAUD rate, WDC interrupt period, and the basic TIMER tick period. CCLK remains constant at approximately 1.0003 MHz.

---

## Interrupt System

### Interrupt Overview

| Vector | Hex | ECU Used? | Trigger Source |
|---|---|---|---|
| IV0 | 0 | N | Unknown (possibly WDT) |
| IV1 | 1 | N | SIN1 serial data ready |
| IV2 | 2 | I (indirect) | IRL̅ pin falling edge |
| IV3 | 3 | I (indirect) | IRP̅ pin |
| IV4 | 4 | Y | ASR3 edge event |
| IV5 | 5 | Y | ASR0 edge event |
| IV6 | 6 | Y* | Manually triggered via IRQLL bit 1 |
| IV7 | 7 | Y | Timer CPR1 match |
| IV8 | 8 | Y | Timer CPR2 match |
| IV9 | 9 | Y | Timer CPR0 match |
| IVA | A | Y | Timer CPR3 match |
| IVB | B | N | Unknown |
| IVC | C | Y | Watchdog timer (WDT) |
| IVD | D | N | Unknown |
| IVE | E | Y | ASR2 edge event |
| IVF | F | Y | NMI (Non-Maskable Interrupt) |

> `I` = used indirectly by examining IRQL bit; `Y*` = triggered by manually setting IRQL bit in code.

### IMASK / IRQL Registers

**IMASK ($2Eh MSB, $2Fh LSB)** — Writing a 1 to a bit enables the corresponding interrupt vector.

| IMASK bit | Signal / Vector |
|---|---|
| 0 | IV9 |
| 1 | IV7 |
| 2 | IV8 |
| 3 | IVA |
| 4 | IVC |
| 5 | IVB (unknown) |
| 6 | IV3 |
| 7 | Global IRQ enable |

**IRQL ($2Ch MSB, $2Dh LSB)** — Interrupt Request Latch. Latches high when an interrupt fires. **Must be cleared during interrupt servicing** or the interrupt will repeatedly retrigger.

| IRQL bit | Signal / Vector |
|---|---|
| 0 | IV9 |
| 1 | IV7 |
| 2 | IV8 |
| 3 | IVA |
| 4 | IVC |
| 5 | IVB (unknown) |
| 6 | IV3 |
| 7 | (global / overflow) |

> Bit F (MSB of combined IRQL:IRQLL) is set when **any** interrupt is triggered.

### Stack Handling on IRQ

On any interrupt, the MCU automatically pushes three items onto the stack in this order (from first pushed to last):

| Stack order | Contents | Size |
|---|---|---|
| 1st (deepest) | PC (points to next instruction after interrupt) | 2 bytes |
| 2nd | Register D (A:B) | 2 bytes |
| 3rd (top, last written) | OCR (CCR) | 1 byte |

The stack pointer **decrements then writes** — it always points to the last datum written.

**Validated by test:** After a call from PC=8022h (`nmi`):
```
rA=20       ; OCR just before NMI
rD=01BB     ; SP right after interrupt (points to last saved item)
[01BB]=20   ; OCR pushed last
[01BC]=12   ; high byte of D
[01BD]=34   ; low byte of D
[01BE]=80   ; high byte of PC
[01BF]=23   ; low byte of PC (= 8023h, next opcode)
```

**Interrupt return** (`reti`) restores all three items and resumes execution.

---

### Interrupt Vector Analysis

#### IVF — NMI (Non-Maskable Interrupt)

In the Toyota ECU, IVF is used to perform a soft **__RESET** by seeding the stack with the reset vector address, then executing `reti`. It also clears the global IRQ mask bit before returning.

```asm
IVf:
    ld s, #01AFh          ; Set SP to known location
    ld d, #0F053h         ; __RESET address
    push d                ; Push PC (return address)
    push d                ; Push reg D
    clr a
    push a                ; Push OCR (CCR = 0)
    clrb bit7, $2Eh       ; Clear Global IRQ mask
    reti                  ; Jump to __RESET
```

> The NMI pin on the MCU is tied to VCC in the ECU (never externally triggered).

#### IVE — ASR2 Edge Event

- Enable: set IMASK bit 6 (`ld d, #1000h; st d, $IMASK`)
- Trigger: ASR2 rising or falling edge (controlled by TAIT bit 4)
- Clear: `clrb bit6, $2Dh` (IRQLL) — must be done during ISR or interrupt fires continuously

#### IVD — ASR1 Edge Event

- Enable: set IMASK bit 5
- Trigger: ASR1 falling edge
- Clear: `clrb bit5, $2Dh` (IRQLL)
- ASR1N delta measured at ~0x420h ticks at 947 Hz, equating to a period of ~1056 µs

#### IVC — Watchdog Timer

- Enable: set IMASK bit 4
- Suspected trigger: WDT overflow
- Timer delta per IVC event: `400h × 4 µs = 4096 µs` (122.07 Hz at ECU WDC setting)
- WDC reads `FFF9h`–`FFFBh` after trigger
- To achieve 0.26 s (basic MIL flash interval), 64 (`40h`) IVC events are required

#### IVA — Timer CPR3 Match

- Enable: set IMASK bit B (`ld d, #0800h`)
- CPR3 is set to `$TIMER + delta` inside the ISR
- Resolution: adding `0001h` to CPR3 = adding `08h` to the 19-bit timer counter = **4 µs**
- IRQL bit D is set on TIMER overflow (19-bit wraparound)

#### IV9 — Timer CPR0 Match

- Enable: set IMASK bit 8 (`ld d, #0100h`)
- Resolution: same as IVA — 4 µs per CPR0 count
- DOUT bits 0 and 4 are latched to LDOUT on each CPR0 trigger (if DOM bits set)

#### IV8 — Timer CPR2 Match

- Enable: set IMASK bit A (`ld d, #0400h`)
- Resolution: 4 µs per CPR2 count
- DOUT bits 2 and 6 latched on CPR2 trigger

#### IV7 — Timer CPR1 Match

- Enable: set IMASK bit 9 (`ld d, #0200h`)
- DOUT bits 1 and 5 latched on CPR1 trigger

#### IV6 — Manual / Software Trigger

- Enable: set IMASKL bit 1
- Triggered manually by IVC and IVE by writing `setb bit1, $2Dh`
- ISR saves and restores X and Y registers around its work

#### IV5 — ASR0 Edge Event

- Enable: set IMASK bit 4 (`ld d, #0010h`)
- Trigger: ASR0 falling edge
- Clear: `clrb bit4, $2Dh`
- Can also wake the CPU from the `wait` instruction

#### IV4 — ASR3 Edge Event

- Enable: set IMASK bit 7 (`ld d, #0080h`)
- Trigger: ASR3 edge (direction controlled by TAIT bit 5)
- Clear: `clrb bit7, $2Dh`
- ASR3 resolution: **4 µs** (TIMERC/8); ASR3 delta measured as `108h` at 947 Hz = 1056 µs period

#### IV3 — IRP̅ Pin

- Enable: set IMASK bit E (`ld d, #4000h`)
- Trigger: IRP̅ pin going low
- Clear: `clrb bit6, $2Ch` (IRQLH)

#### IV2 — IRL̅ Pin

- Enable: set IMASKL bit 0
- Trigger: IRL̅ = 0V; interrupt halts when IRL̅ returns to 5V

#### IV1 — SIN1 Serial Input

- Enable: set IMASK bit 3 (`ld d, #0008h`)
- Trigger: serial data received in buffer (SSD bit 7 set)
- Clear: `clrb bit3, $0Dh`

#### IV0 — Unknown (likely WDT)

- Enable: set IMASKL bit 2
- Timer delta per IV0: `162h × 4 µs = 1416 µs` (353.16 Hz at ECU WDC setting)
- WDC reads `FFF8h` after trigger

---

## Timer System

The main timer is a **19-bit counter** (TIMERC) clocked at 2 MHz (0.5 µs per tick).

**Register mapping to 19-bit TIMERC:**

| Register | Bits captured |
|---|---|
| TIMER ($04h) | TIMERC bits [18:11] — top 8 bits |
| TIMERL ($05h) | TIMERC bits [10:3] |
| TIMER3 ($03h) | TIMERC bits [2:0] — lowest 3 bits |

**Reading TIMER in full (16-bit view):**
- `$TIMER` (as a 16-bit word at $04:$05) represents TIMERC/8
- Adding `0001h` to `$TIMER` is equivalent to adding `0008h` to the raw 19-bit counter = **4 µs**
- The 19-bit counter maximum value is `7FFFFh`; IRQL bit D is set on overflow

**CPR (Compare Register) resolution:**
- CPRx is compared against the 19-bit TIMERC
- Adding `0001h` to CPRx = 4 µs interval
- Adding `0080h` to CPRx ≈ 512 µs interval

**Timer clock:** CCLK ≈ 1.0003 MHz (confirmed by IV0 timing measurement).

---

## ASR (Analogue Signal / Edge Counter) Subsystem

The ASR (Analogue Signal Register) inputs capture a timestamp from the 19-bit timer on each edge event.

### ASR0 and ASR1

- Capture **TIMERC/2** (1 µs resolution) on each edge
- ASR0P / ASR1P: capture on **rising edge**
- ASR0N / ASR1N: capture on **falling edge**
- Reading order matters — allow time for internal latching after the edge before reading ASRxP/N; otherwise the previous value may be read

**Converting ASR capture to absolute time:**
```
TIMERC (19-bit) = (TIMER_word × 8) + TIMER3
TIMERC/2 = ASR capture value
Period (µs) = ASR_delta × 1 µs
```

**Example (verified):**
```
TIMER = 36D4h, TIMER3 = F9h
TIMERC = 36D4h × 8 + F9h = 1B6A8h + F9h = 1B7A1h
TIMERC/2 = DB50h
ASR0P captured = DB4Fh
Difference = 1 (1 µs latency from timer read)
```

### ASR2 and ASR3

- Capture **TIMERC/8** (4 µs resolution)
- Edge direction (rising/falling) is controlled by TAIT bits 4 and 5 respectively
- Writing to ASRx registers has no effect
- ASR3 delta formula: `delta × 4 µs = period`

---

## DOUT / LDOUT / DOM

The DOUT subsystem provides 8 latched output pins (DOUT0–DOUT7) with timer-synchronised latching.

| Register | Address | Description |
|---|---|---|
| LDOUT | $25h | Read-only; reflects actual logic state of DOUT pins |
| DOUT | $26h | Write target data here |
| DOM | $27h | Per-bit latch mode control |

**DOM bit behaviour:**
- `DOM.x = 0`: data written to `DOUT.x` is immediately latched to `LDOUT.x`
- `DOM.x = 1`: data written to `DOUT.x` is held until the associated CPRx event fires (no interrupt needed)

**CPR-to-DOUT mapping (DOM.x = 1):**

| CPR event | DOUT bits latched |
|---|---|
| CPR0 / IV9 | bits 0 and 4 |
| CPR1 / IV7 | bits 1 and 5 |
| CPR2 / IV8 | bits 2 and 6 |
| CPR3 / IVA | bits 3 and 7 |

> Open-drain DOUT pins require external 5V pull-ups to function correctly.

---

## Port A / Port B / Port C / Port D

### Port A ($20h / $21h)

Direction configured via DDRA ($00h). Bits 0–3 are toggled between input and output in ECU code.

| Bit | Direction | Signal (7M-GE ECU) |
|---|---|---|
| 0 | Output | MREL |
| 1 | Output | KNK MCU |
| 2 | Output | KNK MCU |
| 3 | Output | VF1 |
| 4 | Input | G1 (crank sensor) |
| 5 | Input | G2 (crank sensor) |
| 6 | Input | KNK MCU |
| 7 | Input | KNK MCU |

**PORTAL ($21h):** Latched version of PORTA.
- Bits 4–5: latched on **rising** edge
- Bits 6–7: latched on **falling** edge
- Bits 0–3: always read as 1 (not implemented)
- PORTAL is cleared at RESET and at the end of each main loop

### Port B ($22h / $23h)

All bits set to output (DDRB = FFh). PBCS ($23h) is cleared at RESET.

**PBCS status bits (read):**
- PBCS.4 = current logic state of IS̅ pin
- PBCS.5 = current logic state of IRP pin
- Bits 6–7 always read as 1
- IS̅ hi→lo transition sets PBCS.2
- Note: IS̅ is the same physical pin as RD̅; OS̅ is the same pin as WR̅

| Bit | Direction | Signal |
|---|---|---|
| 0 | Output | ISC |
| 1 | Output | ISC |
| 2 | Output | ISC |
| 3 | Output | ISC |
| 4 | Output | KNK MCU |
| 5 | Output | KNK MCU |

### Port C ($28h)

Input only. Logic levels on PCx pins are latched when the register is read.

| Bit | Signal |
|---|---|
| 0 | IDL |
| 1 | DFG / LP |
| 2 | TE1 |
| 3 | NSW |
| 4 | AC |
| 5 | STA |
| 6 | OX1 |
| 7 | IGSW |

### Port D / ASRIN ($29h)

| Bits | Read | Write |
|---|---|---|
| 7–4 | ASR3–ASR0 pin logic status | — (ignored) |
| 3–0 | Latched PD3–PD0 output state | PD3–PD0 output data |

Output only (lower nibble). Upper nibble reflects ASR pin logic at time of register read.

---

## WI Pin and PRAM (Preserved RAM)

RAM from `$80h` to `$9Fh` is designated PRAM — it retains data across ignition-off events as long as Vcc remains at ~5V.

**Pin behaviour:**

| Condition | WI̅ | HALT̅ | INIT̅ | PRAM state |
|---|---|---|---|---|
| IGSW off | Low | Low | Low | Read-only, data preserved |
| IGSW on | High | High | High | R/W accessible |

**RAMST register ($2Ah):**

| Bit | Function |
|---|---|
| 7 | If set at __RESET, PRAM is purged |
| 6 | Checked and set by ECU; function unknown |
| 5 | 0 = PRAM read-only; 1 = PRAM read/write |
| 4 | Unknown |
| 3–0 | Always read as 1 |

- On hard power-up: RAMST.5 is cleared (PRAM is read-only until explicitly unlocked)
- On soft RESET (via INIT̅ pin): RAMST.5 is preserved
- ECU unlocks PRAM by setting bit 5 of RAMST after confirming bit 7 is clear

---

## Serial Communications

### Pins

| Pin | Direction | Function |
|---|---|---|
| SIN0–SIN3 | Input | Serial data input channels |
| SOUT0–SOUT1 | Output | Serial data output channels |
| CLK | — | Serial clock (frequency set by SMRC bits 0–3) |

### SMRC / SIR Register ($07h)

Used for serial setup. Also usable as a simple logic input port (SINx lines readable as port bits).

**ECU serial mode:** `30h` = `0011 0000b` → CLK = 1 MHz  
**Diagnostic reader mode:** `18h` = `0001 1000b` → CLK = 9.6 kHz

**SMRC bits 0–3 clock frequency table:**

| Bits | Frequency |
|---|---|
| 0000 | 1 MHz |
| 0001 | 500 kHz |
| 0010 | 250 kHz |
| 0011 | 125 kHz |
| 0100 | 62.5 kHz |
| 0101 | 31.25 kHz |
| 0110 | 500 kHz |
| 0111 | 62.5 kHz |
| 1000 | 9.6 kHz |
| 1001 | 4.808 kHz |
| 1010 | 2.404 kHz |
| 1011 | 1.202 kHz |
| 1100 | 600 Hz |
| 1101 | 300 Hz |
| 1110 | 62.5 kHz |
| 1111 | 7.813 kHz |

**SIN lines used as configuration inputs (7M-GE ECU):**
- SIN1: JDM = 0, NA = 1 (resistor R606)
- SIN2: A/T = 0, M/T = 1 (resistor R607)
- SIN3: diagnostic line (normally logic 1)

### SSD Register ($2Bh)

| Bit | Function |
|---|---|
| 0 | Set by user to request diagnostic data from RAM above FFh (address extension) |
| 1 | Parity bit 9: 0 = diagnostics channel (space), 1 = ADC channel (mark) |
| 2 | Output channel select: 0 = SOUT0, 1 = SOUT1 |
| 3 | Unknown |
| 4 | DOUT0 buffer empty |
| 5 | Unknown |
| 6 | DIN0 bit 9 status |
| 7 | DIN0 buffer ready (triggers IV1) |

### Serial Frame Format

Each serial frame is **11 bits**: `S [D7..D0] C E`

- S = Start bit
- D7–D0 = Data (transmitted LSB first; read data bytes backwards)
- C = Channel bit (0 = diagnostics, 1 = ADC), controlled by SSD bit 1
- E = End/stop bit

---

## Watch Dog Timer (WDC)

**Address:** $02h  
**ECU value:** `5Ah` = `0101 1010b`

The WDC register controls watchdog timer behaviour. WDC bits 0–7 do not appear to change the interrupt rate — only the final nibble of the WDC read value varies after each interrupt, with no obvious pattern.

IV0 and IVC are both suspected to be triggered by the WDT at rates determined by TAIT bits 0–3.

**TAIT settings and observed IVC / IV0 rates:**

| TAIT [3:0] | IVC rate | IV0 rate |
|---|---|---|
| 0000 | 352.68 Hz | — |
| 0001 | 306.81 Hz | — |
| 0010 | 174.85 Hz | — |
| 0011 | 122.07 Hz | 352.16 Hz |
| 0100 | 32.06 Hz | — |
| 0101 | 21.86 Hz | — |
| 0110 | 308.31 Hz | — |
| 0111 | 43.71 Hz | — |
| 1000 | 6.75 Hz | — |
| 1001 | 3.37 Hz | — |
| 1010 | 1.68 Hz | — |
| 1011 | 0.84 Hz | — |
| 1100 | 0.42 Hz | — |
| 1101 | 0.21 Hz | — |
| 1110 | 43.71 Hz | — |
| 1111 | 5.48 Hz | — |

---

## MIL Light Timing

The MIL (Malfunction Indicator Lamp) flash timing is derived from IVC events.

- IVC period (at ECU WDC setting, TAIT=0011): 4096 µs (122.07 Hz)
- Basic MIL flash interval (0.26 s ON or OFF): **64 (`40h`) IVC events**

**Time delay counter reference (IVC base = 4.096 ms):**

| Time | IVC count (4.096ms) | IVC count (8.192ms) | IVC count (16.392ms) |
|---|---|---|---|
| 0.262 s | 40h | 20h | 10h |
| 0.524 s | 80h | 40h | 20h |
| 0.598 s | 92h | 49h | 24h |
| 1.499 s | 16Eh | B7h | 5Bh |
| 1.573 s | 180h | C0h | 60h |
| 2.621 s | 280h | 140h | A0h |
| 3.670 s | 380h | 1C0h | E0h |
| 4.497 s | 44Ah | 225h | 112h |
| 4.719 s | 480h | 240h | 120h |

---

## Branch Operations — Validated Behaviour

> **Important:** D8X subtraction is **operand-reversed** compared to M68HC11 and 68000.  
> `SUB A, B` → `A = A − B` (not B − A)  
> `CMP #25h, $var` → tests `var − #25h`

### Unsigned comparisons (after `cmp a, #xx` or `cmp #xx, $var`)

| Mnemonic | Branch condition |
|---|---|
| `beq` | operand == #xx |
| `bne` | operand != #xx |
| `bcc` | operand >= #xx |
| `bcs` | operand < #xx |
| `bgt` | operand > #xx (unsigned) |
| `ble` | operand <= #xx (unsigned) |

### Signed comparisons (after `cmp a, #xx`)

| Mnemonic | Branch condition |
|---|---|
| `beq` | operand == #xx |
| `bne` | operand != #xx |
| `blta` | operand < #xx (signed) |
| `blea` | operand <= #xx (signed) |
| `bgta` | operand > #xx (signed) |
| `bgea` / `bge` | operand >= #xx (signed) |
| `bmi` | result < 0 |
| `bpz` | result >= 0 |

### `tbs` instruction

`tbs bitX, varX` tests the specified bit and sets the Z flag: Z=1 if bit is **clear**, Z=0 if bit is **set**.

Commonly used idiom:
```asm
tbs bit0, Status06     ; Z=1 if bit0 cleared
beq loc_E855           ; branch if cleared
clrb bit0, Status06    ; otherwise clear it
```

---

## Bit Operations — Address Mapping

Bit instructions (`setb`, `clrb`, `tbs`, `tbbs`, `tbbc`) encode a 5-bit address in the opcode byte. This 5-bit value is **not** the direct register address — the CPU applies an offset:

| 5-bit opcode value | CPU adds | Effective address range |
|---|---|---|
| 00h – 0Fh | +20h | $20h – $2Fh (I/O registers) |
| 10h – 1Fh | +30h | $40h – $4Fh (RAM) |

**Consequence:** I/O registers $00h–$1Fh are **inaccessible** to bit operations.

**Example:** opcode `75 09` does not mean `setb bit1, $09h` — it means `setb bit1, $29h` (PORTD_ASRIN).

This was a known source of confusion in early disassembly attempts. The TASM assembler was updated with `DBIT` and `DREL` directives to handle this address transformation automatically.

---

## Division Instruction — Validated Behaviour

`div d, #xx` / `div d, $xx` / `div d, $xxxx`

**Operation:** B ← D ÷ operand; A ← D mod operand

**Constraint:** The quotient must fit in 8 bits (≤ FFh). If A (high byte of D) ≥ operand, the division is a **NOP** and C flag is set to 1.

| Condition | Result |
|---|---|
| A < operand | B = quotient, A = remainder, C = 0 |
| A >= operand | NOP (D unchanged), C = 1 |
| Divide by zero | Always NOP (A >= 0 is always true), C = 1 |

**Test cases:**
```
39A8h / 40h = E6h remainder 28h  → rD = 28E6h, C=0  ✓
61A8h / 40h  (A=61h >= 40h)     → NOP, C=1          ✓ (quotient 186h > FFh)
xxxx / 00h                       → NOP, C=1          ✓
```

> Divide-by-zero (`div d, #00h`) is used in ECU code as a deliberate **delay / NOP** instruction.

---

## 7M-GE Knock MCU Interface

The KNK (Knock) MCU is a separate D8X in DIP42 package that interfaces to the main MCU via the following signals:

| Signal | Direction (main MCU) | Function |
|---|---|---|
| IRL̅ | Input | Timing data from KNK MCU |
| PA1 | Output | Command: clr-set pulse after start when RPM > 400 |
| PA2 | Output | Command: clr at TDC, set at 30° past TDC; clr during ADC ch1 read if PRAM read-only |
| PB4 | Output | Command to KNK MCU |
| PB5 | Output | Command to KNK MCU |
| PA6 | Input | Data from KNK MCU |
| PA7 | Input | Data from KNK MCU |
| INIT̅ | — | Shared reset line |

---

*Sections above derived from Denso 8X (7433) test notes by Jon Hacker and Henri de Rauly.*  
*Original instruction set reference from `toshiba-8x-datasheet.pdf` v0.01 by David Sobon (30-Apr-2011).*

