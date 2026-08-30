; ---------------------------------------------------------------------------
; knock_mcu_update: communicate with knock detection MCU on every NE pulse
;
; Called from int_vector_e_ne (24x per revolution).
; Performs two tasks depending on which NE position (0..5) we are at:
;
; POSITION 0 (ne_count bits 2..0 = 0):
;   - If TDC cylinder 1 (ne_count = 0x00): assert PORTB.1 high
;   - Other cylinders at position 0: de-assert PORTB.1 (clear)
;   - Read 3 knock data bits from PORTB into var_knock_info:
;       PORTB.5 high  -> knock_info bit 3
;       PORTB.4 low   -> knock_info bit 1
;       PORTB.3 low   -> knock_info bit 0
;
; POSITION 1 (ne_count bits 2..0 = 1):
;   - Send handshake pulse: PORTB.1 low then PORTB.0 high
;
; POSITION 2 (ne_count bits 2..0 = 2):
;   - Decode accumulated var_knock_info and process:
;       Shift/rotate extracts 3-bit knock level:
;         V clear           -> knock detected (RPM 700-7200 check, error accumulation)
;         V set + C set     -> no knock (clear error flags)
;         V set + C clear   -> borderline knock (RPM 2850-7200, increment counter)
;       Persistent knock (count >= 3) + G1/G2 error -> set knock sensor error flags
;       Excessive knock events (>= 12 cylinders) -> reset knock MCU via DOUT.2 pulse
;
; POSITIONS 3..5: nothing done, return immediately.
;
; var_knock_info is cleared after decode (position 2) and re-accumulated
; over the next cycle at positions 0 and 1.
;
; Knock MCU reset: DOUT.2 pulsed low for ~12 us (3 x div-by-zero NOP delays)
; RPM thresholds (var_rpm_x_5p12 units): 0x0E=700 RPM, 0x39=2850, 0x90=7200
; ---------------------------------------------------------------------------

knock_mcu_update:						; CODE XREF: ROM:int_vector_e_ne_F04Fp
				ld	b, var_ne_count		; B = current NE position counter
								; bits 3..0: position (0..5), bits 7..4: cylinder (0x00/10/20/30)
				ld	a, var_knock_info		; A = accumulated knock data from previous read
				cmpz	b				; ne_count == 0x00 (TDC cylinder 1, position 0)?
				bne	loc_F47B			; No: check other positions

				setb	bit1, PORTB			; TDC cyl 1: assert PORTB.1 high to knock MCU
				bra	loc_F481			; Jump to read knock data bits

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

loc_F47B:							; CODE XREF: knock_mcu_update+6j
; Check position bits 2..0 to identify NE position
				and	b, #07h			; B = position bits 2..0 (0..5 within cylinder)
				bne	loc_F484			; Not position 0: check position 1

				clrb	bit1, PORTB			; Position 0 (non-TDC cylinder): de-assert PORTB.1

loc_F481:							; CODE XREF: knock_mcu_update+Aj
				jmp	loc_F515			; Read knock data bits from PORTB

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

loc_F484:							; CODE XREF: knock_mcu_update+Ej
; Position 1: send handshake pulse, do not read knock data
				dec	b				; B-- (B=0 if was position 1)
				bne	loc_F48A			; Not position 1: check position 2
				jmp	loc_F50B			; Position 1: send handshake

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

loc_F48A:							; CODE XREF: knock_mcu_update+16j
; Position 2: decode knock data, or return for positions 3..5
				dec	b				; B-- (B=0 if was position 2)
				beq	loc_F490			; Position 2: go decode knock data
				jmp	locret_F529			; Positions 3..5: nothing to do, return

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

; Position 2: gate checks before reading knock level
loc_F490:							; CODE XREF: knock_mcu_update+1Cj
				tbbs	bit5, unk_40, loc_F508		; Skip if init guard flag set
				tbbc	bit2, DOUT, loc_F508		; Skip if knock MCU in reset (DOUT.2 = 0)
				tbbs	bit5, IRQLL, loc_F508		; Skip if ASR0 interrupt pending
				tbbs	bit5, PORTD_ASRIN, loc_F508	; Skip if ASR3 input pin high
				cmp	#7Ah, var_4ms_cnt_BE
				ble	loc_F508			; Skip if engine not yet settled (4ms count < 0x7A)
				cmp	#7Ah, var_4ms_starter_cnt_C0
				ble	loc_F508			; Skip if just started (starter count < 0x7A)

; Decode accumulated var_knock_info: extract 3-bit knock level via shift/rotate
; var_knock_info bits: bit3=PORTB.5, bit1=PORTB.4 low, bit0=PORTB.3 low
				mov	a, b				; B = var_knock_info (previous)
				tbbc	bit5, PORTB, loc_F4AC		; If PORTB.5 low, skip add
				add	b, #04h				; PORTB.5 high: set bit2 of knock level

loc_F4AC:							; CODE XREF: knock_mcu_update+38j
; shr x3 + rorc: extract knock level, set V/C flags for branching
;   V clear        = knock detected
;   V set + C set  = no knock
;   V set + C clr  = borderline/severe knock
				shr	b				; knock level >> 1
				shr	b				; knock level >> 1
				shr	b				; knock level >> 1 (3-bit level now in C and V flags)
				rorc	b				; rotate through carry: V set if sign change
				bvc	loc_F4D0			; V clear: knock detected
				bcs	loc_F4FF			; V set + C set: no knock, clear errors

; V set + C clear: borderline knock signal, RPM window 2850-7200
				cmp	#39h, var_rpm_x_5p12		; RPM < 2850?
				bcs	loc_F508			; Yes: ignore
				cmp	#90h, var_rpm_x_5p12		; RPM >= 7200?
				bcc	loc_F508			; Yes: ignore
				cmp	#03h, var_cnt_knock_signal	; Knock signal count >= 3?
				bcc	loc_F4C7			; Yes: persistent knock, check for error
				inc	var_cnt_knock_signal		; No: accumulate knock counter
				bra	loc_F508

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

; Persistent knock (count >= 3): set error if G1/G2 error also present
loc_F4C7:							; CODE XREF: knock_mcu_update+52j
				tbbc	bit7, var_diag_errors_5, loc_F508 ; Skip if no G1/G2 error
				setb	bit5, var_flags_46		; Set knock sensor error flag
				setb	bit0, var_error_flags2		; Set knock sensor error (diag)
				bra	loc_F508

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

; V clear: knock detected, RPM window 700-7200
loc_F4D0:							; CODE XREF: knock_mcu_update+41j
				cmp	#0Eh, var_rpm_x_5p12		; RPM < 700?
				bcs	loc_F508			; Yes: ignore
				cmp	#90h, var_rpm_x_5p12		; RPM >= 7200?
				bcc	loc_F508			; Yes: ignore
				ld	b, nv_diag_errors_1
				cmpb	b, #80h				; G1/G2 diag error active (only bit7 = NE error)?
				bne	loc_F508			; Yes: ignore knock reading (crank sync unreliable)

; Track how many consecutive cylinders have detected knock
				and	a, #0F0h			; A = cylinder count nibble only
				add	a, #10h				; Increment cylinder knock counter
				cmp	a, #0C0h			; Reached limit (0xC0 = 12 cylinders)?
				bcs	loc_F4ED			; Not yet: check for reset threshold
				setb	bit4, var_diag_errors_5		; Too many knock cylinders: set diagnostic error
				clr	a				; Reset cylinder knock counter
				bra	knock_mcu_reset			; Reset knock MCU hardware

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

; Check if 6 consecutive knock cylinders reached (0x60)
loc_F4ED:							; CODE XREF: knock_mcu_update+77j
				cmp	a, #60h				; 6 consecutive knock cylinders?
				bne	loc_F509			; No: save updated knock_info and return


; Reset knock MCU: pulse DOUT.2 low for ~12 us (3 x div-by-zero delay)
knock_mcu_reset:						; CODE XREF: knock_mcu_update+7Cj
				clrb	bit2, DOUT			; Assert knock MCU reset (DOUT.2 low)
				div	d, #00h				; Delay ~4 us (div by zero = NOP with latency)
				div	d, #00h				; Delay ~4 us
				div	d, #00h				; Delay ~4 us
				setb	bit2, DOUT			; Release knock MCU reset (DOUT.2 high)
				setb	bit5, var_flags_46		; Set knock sensor error flag
				bra	loc_F509

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

; V set + C set: no knock detected - clear error flags
loc_F4FF:							; CODE XREF: knock_mcu_update+43j
				tbbs	bit0, var_error_flags2, loc_F508 ; Skip if knock error latched in diag
				clrb	bit5, var_flags_46		; Clear knock sensor error
				clrb	bit4, var_diag_errors_5		; Clear knock management diag error
				clr	var_cnt_knock_signal		; Reset knock accumulation counter

loc_F508:							; CODE XREF: knock_mcu_update:loc_F490j
								; knock_mcu_update+24j	...
				clr	a				; A = 0: clear knock_info accumulator for next cycle

loc_F509:							; CODE XREF: knock_mcu_update+80j
								; knock_mcu_update+8Ej
				bra	loc_F526			; Save A to var_knock_info and return

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

; Position 1 handshake: pulse PORTB.1 low then PORTB.0 high
loc_F50B:							; CODE XREF: knock_mcu_update+18j
				clrb	bit1, PORTB			; PORTB.1 low (de-assert)
				bra	loc_F511

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ
				setb	bit1, PORTB			; PORTB.1 high (part of pulse sequence)

loc_F511:							; CODE XREF: knock_mcu_update+9Ej
				setb	bit0, PORTB			; PORTB.0 high (handshake pulse complete)
				bra	locret_F529			; Return without updating var_knock_info

; ÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄÄ

; Read 3-bit knock data from PORTB into var_knock_info (positions 0 and 1)
loc_F515:							; CODE XREF: knock_mcu_update:loc_F481j
				tbbc	bit5, PORTB, loc_F51A		; PORTB.5 low? Skip bit3
				or	a, #08h				; PORTB.5 high: set knock_info bit3 (level MSB)

loc_F51A:							; CODE XREF: knock_mcu_update:loc_F515j
				clrb	bit0, PORTB			; Strobe PORTB.0 low (clock to knock MCU)
				tbbs	bit4, PORTB, loc_F521		; PORTB.4 high? Skip bit1
				or	a, #02h				; PORTB.4 low: set knock_info bit1

loc_F521:							; CODE XREF: knock_mcu_update+ADj
				tbbs	bit3, PORTB, loc_F526		; PORTB.3 high? Skip bit0
				or	a, #01h				; PORTB.3 low: set knock_info bit0 (level LSB)

loc_F526:							; CODE XREF: knock_mcu_update:loc_F509j
								; knock_mcu_update:loc_F521j
				st	a, var_knock_info		; Save accumulated knock data for next decode cycle

locret_F529:							; CODE XREF: knock_mcu_update+1Ej
								; knock_mcu_update+A4j
				ret

; End of function knock_mcu_update
