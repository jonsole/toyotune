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

				.org	0040h
unk_40:			.block	1		; DATA XREF: sub_8063+14w
unk_41:			.block	1		; DATA XREF: sub_8063+8w
unk_42:			.block	1		; DATA XREF: sub_8063+16w

				.org	8000h
__RESET:
				ld		#06h, $1Fh	; Mode control Register
				di
				ld		s, #01BFh

				ld		#FFh, $DDRA
				ld		#00h, $DDRB
				
				ld		a, #00h			
loop:			st		a, $PORTA
				
				inc		a
				bra		loop
					
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
				.dw __RESET		; Processor reset

				.end
