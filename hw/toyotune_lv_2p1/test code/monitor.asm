;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;;
; TASM  test2 source
; Test operation of the Denso 8X Processor:  D8X
;
; This program test the difference between opcodes 0A and 3A shows as equal
; in the Kashi disassembler (Zero Page Direct)
; stxz	0A%8u	st x, $&0a	Store Register contents to memory M[op.2] = X
; stxz	3A%8u	st x, $&0a	Store Register contents to memory M[op.1] = X	
; ---------------------------------------------------------------------------
; Segment type: Pure code
;.segment code
		.msfirst		; Big Endian processor

_DDRA           .equ 00h                ; Port A i/o config
_DDRB           .equ 01h                ; Port B i/o config
_WDC            .equ 02h                ; watch dog timer
_TIMER3         .equ 03h                ; Timer LSB (bit0~bit2)
_TIMER          .equ 04h                ; Timer MSB (bit11~bit18)
_TIMERL         .equ 05h                ; Timer LSB (bit3~bit10)
_SIDR_SODR      .equ 06h                ; Serial Input/Output Data Register
_SMRC_SIR       .equ 07h                ; Serial Master Register Control
_CPR0           .equ 08h                ; Timer comparison #0 MSB
_CPR0L          .equ 09h                ; Timer comparison #0 LSB
_CPR1           .equ 0Ah                ; Timer comparison #1 MSB
_CPR1L          .equ 0Bh                ; Timer comparison #1 LSB
_CPR2           .equ 0Ch                ; Timer comparison #2 MSB
_CPR2L          .equ 0Dh                ; Timer comparison #2 LSB
_CPR3           .equ 0Eh                ; Timer comparison #3 MSB
_CPR3L          .equ 0Fh                ; Timer comparison #3 LSB
_ASR0P          .equ 10h                ; ASR0 pos edge counter value MSB
_ASR0PL         .equ 11h                ; ASR0 pos edge counter value LSB
_ASR0N          .equ 12h                ; ASR0 neg edge counter value MSB
_ASR0NL         .equ 13h                ; ASR0 neg edge counter value LSB
_ASR1P          .equ 14h                ; ASR1 pos edge counter value MSB
_ASR1PL         .equ 15h                ; ASR1 pos edge counter value LSB
_ASR1N          .equ 16h                ; ASR1 neg edge counter value MSB
_ASR1NL         .equ 17h                ; ASR1 neg edge counter value LSB
_ASR2           .equ 18h                ; ASR2 edge counter value MSB
_ASR2L          .equ 19h                ; ASR2 edge counter value LSB
_ASR3           .equ 1Ah                ; ASR3 edge counter value MSB
_ASR3L          .equ 1Bh                ; ASR3 edge counter value LSB
_OMODE          .equ 1Fh                ; Mode control Register
_PORTA          .equ 20h                ; Port A Data Register
_PORTAL         .equ 21h                ; Port A Latch
_PORTB          .equ 22h                ; Port B Data Register
_PBCS           .equ 23h                ; Port B Control Register
_TAIT           .equ 24h                ; Timer ASR Control
_LDOUT          .equ 25h                ; Latch DOUT
_DOUT           .equ 26h                ; DOUT Data Register
_DOM            .equ 27h                ; DOUT Control Register
_PORTC          .equ 28h                ; Port C Data Register
_PORTD_ASRIN    .equ 29h                ; Port D Data Register / ASR Input Data
_RAMST          .equ 2Ah                ; Built-in RAM status
_SSD            .equ 2Bh                ; Serial Status Data Register
_IRQL           .equ 2Ch                ; Interrupt Request Flag MSB
_IRQLL          .equ 2Dh                ; Interrupt Request Flag LSB
_IMASK          .equ 2Eh                ; Interrupt Request Mask MSB

;============================================================================================
; RAM segment
hex_buf			.block	3
line_buf		.block  80
line_idx		.block	1
cmd_word_buf	.block	2

;============================================================================================
; ROM segment
				.org	0C000h
__RESET:		ld		#02h,$_OMODE	; change to mode 2
				di    					; disable interrupts
				ld		s,#01bfh		; set stack to top of RAM
				ld		#18h,$_SMRC_SIR	; set SMRC cintrol reg to 0001 1000
				ld		y,#msg_start	; pointer to startup message string
				bsr		printf			; call printf to dump to terminal
				
edit_start:		ld		a,#00h
				st		a,line_idx		; reset line index
				ld		y,line_buf		; get address of line buffer
				st		a,[y]			; empty buffer
				
edit_loop:		ld		y,line_buf		; get address of line buffer
				ld		b,line_idx		; get index into buffer
				add		y,b				; calculate indexed address
				bsr		getc			; get character	
				cmp		a,#8			; check if it's del
				beq		edit_del		; if it is jump to handle it seperately
				cmp		a,#0Ah			; check if it's enter
				beq		edit_done		; if it is jump to exit editing loop
				st		a,[y]			; store character in buffer
				bsr		putc			; echo character
				ld		a,#00h			; 
				st		a,[y]			; ensure buffer is null terminated
				ld		b,line_idx		; get index in buffer
				inc		b				; increment index
				st		b,line_idx		; store new index
				bra		edit_loop		; loop back to get next character
				
edit_del:		bsr		putc			; echo character
				ld		b,line_idx		; get index into buffer
				dec		b				; decrement index
				st		b,line_idx	    ; store new index
				ld		y,line_buf		; get address of line buffer
				add		y,b				; calculate indexed address
				ld		a,#00h			; 
				st		a,[y]			; ensure buffer is null terminated
				bra		edit_loop		; loop back to get next character

edit_done:	    bsr		putc			; echo character

				ld		y,line_buf
				ld		d,[y]
				cmp		d,#7262h
				beq		cmd_read_byte
				cmp		d,#7277h
				beq		cmd_read_word
				cmp		d,#7762h
				beq		cmd_write_byte
				cmp		d,#7777h
				beq		cmd_write_word
				bra		cmd_error

cmd_read_byte:	bsr		cmd_skip_ws
				bcs		cmd_error
				bsr		cmd_get_word
				bcs		cmd_error
				ld		y,d
				ld		a,[y]
				bsr		putbyte
				bra		cmd_done

cmd_read_word:  bsr		cmd_skip_ws
				bcs		cmd_error
				bsr		cmd_get_word
				bcs		cmd_error
				ld		y,d
				ld		d,[y]
				bsr		putword
				bra		cmd_done

cmd_write_byte: bsr		cmd_skip_ws
				bcs		cmd_error
				bsr		cmd_get_word
				bcs		cmd_error
				st		d,cmd_word_buf
				bsr		cmd_skip_ws
				bcs		cmd_error
				bsr		cmd_get_word
				bcs		cmd_error
				ld		y,cmd_word_buf
				st		a,[y]
				bra		cmd_done

cmd_write_word: bsr		cmd_skip_ws
				bcs		cmd_error
				bsr		cmd_get_word
				bcs		cmd_error
				st		d,cmd_word_buf
				bsr		cmd_skip_ws
				bcs		cmd_error
				bsr		cmd_get_word
				bcs		cmd_error
				ld		y,cmd_word_buf
				st		d,[y]
				bra		cmd_done

cmd_error:		ld		y,#msg_error
				bsr		printf
				bra		edit_start

cmd_done:		ld		a,#0Dh
				bsr		putchar
				ld		a,#0Ah
				bsr		putchar
				bra		edit_start

cmd_skip_ws:	ld		a,[y]
				cmp		a,#20h
				bne		cmd_skip_err
cmd_skip_loop:	ld		a,[y]
				cmp		a,#20h
				beq		cmd_skip_loop
				
cmd_skip_err:	setc
				ret
				

;============================================================================================
getc: 			; get character function
				ld		b,$_SSD			; load serial status data reg into b 
				and		b,#80h			; mask (bit 7)  1000 0000
				beq		get_c			; loop until bit 8 set
				ld		a,$_SIDR_SODR	; load character from SIDR into a
				ret

;============================================================================================
putc:			; put character function
				ld		b,$_SSD			; load serial status data reg into b
				and		b,#20h			; mask (bit 5)  0010 0000
				beq		putc			; loop until bit 6 set
				st		a,$_SIDR_SODR	; store character from a into SODR
				ret   			

;============================================================================================
putstr:
				ld		a, [y]			; load first char of string pointed to by y 
				cmpz	a				; is it the null terminator
				beq		putstr_exit		; yes return
				bsr		putc 			; no, write to output stream and repeat
				bra		putstr			; jump back to handle next character
putstr_exit:	ret
 
;============================================================================================
putbyte:
				push	a				; push a on stack
				ld		y,#hex_code		; base of hex conversion table
				and		a,#0Fh			; isolate low nibble
				add		y,a				; compute pointer into table
				ld		a,[y]			; load ascii char
				st		a,$hex_buf+1	; store ascii char for low nibble in RAM variable hex_buf
				pull	a				; repeat for high nibble
				ld		y,#hex_code		; base of hex conversion table
				shr		a				; shift high nibble into bottom nibble
				shr		a				;
				shr		a				;
				shr		a				;
				add		y,a				; compute pointer into table
				ld		a,[y]			; load ascii char
				st		a,$hex_buf 		; store ascii char for high nibble in RAM variable hex_buf
				clr		$hex_buf+2		; add end of string terminator in hex_buf
				ld		y,#hex_buf		; point y to ascii string
				bra		putstr 			; stream out string		

;============================================================================================
;DATA   

msg_start:		.text	"\r\nDenso 8X Monitor v1.0\r\n\000"
msg_error:		.text   "error\r\n\000"
msg_done:		.text	"\r\r\000"
hex_code		.text	"0123456789ABCDEF"

show_int_vect:	push	a
				ld		y,#msg_int_vect
				bsr		putstr
				pop		a
				bsr		putbyte
				ld		a,#0Dh
				bsr		putchar
				ld		a,#0Ah
				bsr		putchar
				pop		y
				pop		x
				reti
				
int_vect_0:		push	x
				push	y
				clrb    bit2, IRQLL
				ld		a,#0
				bra		show_int_vect
				
int_vect_1:		push	x
				push	y
				clrb    bit3, IRQLL
				ld		a,#1
				bra		show_int_vect

int_vect_2:		push	x
				push	y
				clrb    bit0, IRQLL
				ld		a,#2
				bra		show_int_vect

int_vect_3:		push	x
				push	y
				clrb    bit6, IRQL
				ld		a,#3
				bra		show_int_vect

int_vect_4:		push	x
				push	y
				clrb    bit7, IRQLL
				ld		a,#4
				bra		show_int_vect

int_vect_5:		push	x
				push	y
				clrb    bit4, IRQLL
				ld		a,#5
				bra		show_int_vect

int_vect_6:		push	x
				push	y
				clrb    bit1, IRQLL
				ld		a,#6
				bra		show_int_vect

int_vect_7:		push	x
				push	y
				clrb    bit1, IRQL
				ld		a,#7
				bra		show_int_vect

int_vect_8:		push	x
				push	y
				clrb    bit2, IRQL
				ld		a,#8
				bra		show_int_vect
				
int_vect_9:		push	x
				push	y
				clrb    bit0, IRQL
				ld		a,#9
				bra		show_int_vect

int_vect_a:		push	x
				push	y
				clrb    bit3, IRQL
				ld		a,#0Ah
				bra		show_int_vect

int_vect_b:		push	x
				push	y
				clrb    bit5, IRQL
				ld		a,#0Bh
				bra		show_int_vect

int_vect_c:		push	x
				push	y
				clrb    bit4, IRQL
				ld		a,#0Ch
				bra		show_int_vect

int_vect_d:		push	x
				push	y
				clrb    bit5, IRQLL
				ld		a,#0Dh
				bra		show_int_vect

int_vect_e:		push	x
				push	y
				clrb    bit6, IRQLL
				ld		a,#0Eh
				bra		show_int_vect

int_vect_f:		push	x
				push	y
				ld		a,#0Fh
				bra		show_int_vect
				
		
				.org 0FFDEh
IV0:			.dw int_vect_0			; External interrupt 0
IV1:			.dw int_vect_1          ; External interrupt 1
IV2:			.dw int_vect_2			; External interrupt 2
IV3:			.dw int_vect_3			; External interrupt 3
IV4:			.dw int_vect_4			; External interrupt 4
IV5:			.dw int_vect_5			; External interrupt 5
IV6:			.dw int_vect_6			; External interrupt 6
IV7:			.dw int_vect_7			; External interrupt 7
IV8:			.dw int_vect_8			; External interrupt 8
IV9:			.dw int_vect_9			; External interrupt 9
IVa:			.dw int_vect_a			; External interrupt a
IVb:			.dw int_vect_b			; External interrupt b
IVc:			.dw int_vect_c			; External interrupt c
IVd:			.dw int_vect_d			; External interrupt d
IVe:			.dw int_vect_e			; External interrupt e
IVf:			.dw int_vect_f			; External interrupt f
RESET:			.dw __RESET             ; Processor reset
				.end

