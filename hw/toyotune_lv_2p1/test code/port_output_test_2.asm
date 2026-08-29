				.msfirst		; Big Endian processor
				
				.org	0000h

DDRA:			.block	1		; Port A i/o config
DDRB:			.block	1		; Port B i/o config
WDC:			.block	1		; watch	dog timer
TIMER3:			.block	1		; Timer	LSB (bit0~bit2)
TIMER:			.block	1		; Timer	MSB (bit11~bit18)
TIMERL:			.block	1		; Timer	LSB (bit3~bit10)
SIDR_SODR:		.block	1		; Serial Input/Output Data Register
SMRC_SIR:		.block	1		; Serial Master	Register Control
CPR0:			.block	1		; Timer	comparison #0 MSB
CPR0L:			.block	1		; Timer	comparison #0 LSB
CPR1:			.block	1		; Timer	comparison #1 MSB
CPR1L:			.block	1		; Timer	comparison #1 LSB
CPR2:			.block	1		; Timer	comparison #2 MSB
CPR2L:			.block	1		; Timer	comparison #2 LSB
CPR3:			.block	1		; Timer	comparison #3 MSB
CPR3L:			.block	1		; Timer	comparison #3 LSB
ASR0P:			.block	1		; ASR0 pos edge	counter	value MSB
ASR0PL:			.block	1		; ASR0 pos edge	counter	value LSB
ASR0N:			.block	1		; ASR0 neg edge	counter	value MSB
ASR0NL:			.block	1		; ASR0 neg edge	counter	value LSB
ASR1P:			.block	1		; ASR1 pos edge	counter	value MSB
ASR1PL:			.block	1		; ASR1 pos edge	counter	value LSB
ASR1N:			.block	1		; ASR1 neg edge	counter	value MSB
ASR1NL:			.block	1		; ASR1 neg edge	counter	value LSB
ASR2:			.block	1		; ASR2 edge counter value MSB
ASR2L:			.block	1		; ASR2 edge counter value LSB
ASR3:			.block	1		; ASR3 edge counter value MSB
ASR3L:			.block	1		; ASR3 edge counter value LSB
				.block	1
				.block	1
				.block	1
OMODE:			.block	1		; Mode control Register
PORTA:			.block	1		; Port A Data Register
PORTAL:			.block	1		; Port A Latch
PORTB:			.block	1		; Port B Data Register
PBCS:			.block	1		; Port B Control Register
TAIT:			.block	1		; Timer	ASR Control
LDOUT:			.block	1		; Latch	DOUT
DOUT:			.block	1		; DOUT Data Register
DOM:			.block	1		; DOUT Control Register
PORTC:			.block	1		; Port C Data Register
PORTD_ASRIN:	.block	1		; Port D Data Register / ASR Input Data
RAMST:			.block	1		; Built-in RAM status
SSD:			.block	1		; Serial Status	Data Register
IRQL:			.block	1		; Interrupt Request Flag MSB
IRQLL:			.block	1		; Interrupt Request Flag LSB
IMASK:			.block	1		; Interrupt Request Mask MSB

				.org	0c000h

__RESET:		ld		#02h, $OMODE	      	; Change to mode 2 (external)
				di
				
				ld		s, #01BFh			; Set stack pointer
				ld		#18h, $SMRC_SIR		; Set SMRC cintrol reg to 0001 1000
				
				ld		a, #'H'				; Stream out A char
				bsr		PUTC
				ld		a, #'E'				; Stream out A char
				bsr		PUTC
				ld		a, #'L'				; Stream out A char
				bsr		PUTC
				ld		a, #'L'				; Stream out A char
				bsr		PUTC
				ld		a, #'O'				; Stream out A char
				bsr		PUTC
				ld		a, #13				; Stream out A char
				bsr		PUTC
				ld		a, #10				; Stream out A char
				bsr		PUTC

				ld		#0Fh, $DDRA			; Set PA[3..0] to outputs, PA[7..4] to inputs
				ld		#0Fh, $DDRB			; Set PB[3..0] to outputs, PB[7..4] to inputs
				
				ld		a, #00h			
loop:			ld		b, #0ffh
				st		b, $CPR0			; Write to internal register, to check CPLD doesn't trigger register access
				st		b, $40h				; Write to internal RAM, to check CPLD doesn't trigger register access
				st		a, $PORTA			; Write to PORTA to generate pattern on PA pins
				st		a, $PORTB			; Write to PORTB to generate pattern on PB pins
				inc		a
				bra		loop
				
PUTC:			ld		b, $SSD				; Load serial status data reg into B
				and		b, #20h				; Mask (bit 5)  0010 0000
				beq		PUTC				; Loop until bit 6 set
				st		a, $SIDR_SODR		; Store character from A into SODR
				ret   			
									
				.org	0FFDEh
			
IV0:			.dw 0FFFFh		; External interrupt 0
IV1:			.dw 0FFFFh		; External interrupt 1
IV2:			.dw 0FFFFh		; External interrupt 2
IV3:			.dw 0FFFFh		; External interrupt 3
IV4:			.dw 0FFFFh		; External interrupt 4
IV5:			.dw 0FFFFh		; External interrupt 5
IV6:			.dw 0FFFFh		; External interrupt 6
IV7:			.dw 0FFFFh		; External interrupt 7
IV8:			.dw 0FFFFh		; External interrupt 8
IV9:			.dw 0FFFFh		; External interrupt 9
IVa:			.dw 0FFFFh		; External interrupt a
IVb:			.dw 0FFFFh		; External interrupt b
IVc:			.dw 0FFFFh		; External interrupt c
IVd:			.dw 0FFFFh		; External interrupt d
IVe:			.dw 0FFFFh		; External interrupt e
IVf:			.dw 0FFFFh		; External interrupt f
RESET			.dw __RESET

				.end
