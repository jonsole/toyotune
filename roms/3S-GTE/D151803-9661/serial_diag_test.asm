                        .org 0000h
                        
DDRA:                   .block 1
DDRB:                   .block 1
WDC:                    .block 1
TIMER3:                 .block 1
TIMER:                  .block 1
TIMERL:                 .block 1
SIDR_SODR:              .block 1
SMRC_SIR:               .block 1
CPR0:                   .block 1
CPR0L:                  .block 1
CPR1:                   .block 1
CPR1L:                  .block 1
CPR2:                   .block 1
CPR2L:                  .block 1
CPR3:                   .block 1
CPR3L:                  .block 1
ASR0P:                  .block 1
ASR0PL:                 .block 1
ASR0N:                  .block 1
ASR0NL:                 .block 1
ASR1P:                  .block 1
ASR1PL:                 .block 1 
ASR1N:                  .block 1 
ASR1NL:                 .block 1 
ASR2:                   .block 1
ASR2L:                  .block 1
ASR3:                   .block 1
ASR3L:                  .block 1
UNK1C:                  .block 1
UNK1D:                  .block 1
                        .block 1
OMODE:                  .block 1
PORTA:                  .block 1
PORTAL:                 .block 1
PORTB:                  .block 1
PBCS:                   .block 1
TAIT:                   .block 1
LDOUT:                  .block 1
DOUT:                   .block 1
DOM:                    .block 1
PORTC:                  .block 1
PORTD_ASRIN:            .block 1
RAMST:                  .block 1
SSD:                    .block 1
IRQL:                   .block 1
IRQLL:                  .block 1
IMASK:                  .block 1
IMASKL:                 .block 1
                        .block 1
                        .block 1
                        .block 1
                        .block 1
UNK34:                  .block 1
                        .block 1
                        .block 1
                        .block 1
CPR4:                   .block 1
CPR4L:                  .block 1
CPR5:                   .block 1
CPR5L:                  .block 1
CPR6:                   .block 1
CPR6L:                  .block 1
CPR7:                   .block 1
CPR7L:                  .block 1

                        .org 0040h
                        
serial_diag_address:    .block 2
serial_diag_data:       .block 2
serial_diag_mode:       .block 1

                        .org 0050h
counter1:               .block 2
                        .block 1
counter2:               .block 2

                        .org 8000h

                        #include "diag_ram_write.asm"

; Unhandled interrupt vector
int_vector_unhandled:
                        bra int_vector_unhandled

; Timer interrupt handler
int_vector_c_timer:                                     
                        clrb    bit4, IRQL              ; Clear IRQL.4, interrupt vector C latch
                        push    x
                        push    y

                        ld      d, counter1
                        add     d,#1
                        st      d, counter1
                        
                        ld      d, counter2
                        add     d,#1
                        st      d, counter2

                        jsr     serial_diag_extended                     

                        ld      a, #40h 
delay:                  dec     a
                        bne     delay

                        pull    y
                        pull    x

                        reti

; Reset vector
reset_vector:
                        ld      #02h, OMODE             ; Mode control Register
                        di
                        
                        ld      #30h, SMRC_SIR          ; Set SIN0 BAUD rate to 1MHz
                        ld      #3Bh, TAIT              ; Set watchdog timer rate
                        ld      s, #02FFh               ; Reset stack pointer

                        ld      d, #1000h
                        st      d, IMASK                ; Enable interrupt vector C

                        ld      d, #55aah
                        st      d, counter1
                        st      d, counter2
                        
						ld		a, #0DAh
						st		a, serial_diag_mode
                        ei                              ; Globally enable interrupts

main_loop:
                        ; Just wait, diagnostics will called from interrupt vector C
                        bra     main_loop
                        

                        .org 0FFDAh
rom_checksum:           .dw 148Eh
rom_version:            .dw 1234h                       

                        .org 0FFDEh
                        .dw int_vector_unhandled        ; External interrupt 0
                        .dw int_vector_unhandled        ; External interrupt 1
                        .dw int_vector_unhandled        ; External interrupt 2
                        .dw int_vector_unhandled        ; External interrupt 3
                        .dw int_vector_unhandled        ; External interrupt 4
                        .dw int_vector_unhandled        ; External interrupt 5
                        .dw int_vector_unhandled        ; External interrupt 6
                        .dw int_vector_unhandled        ; External interrupt 7
                        .dw int_vector_unhandled        ; External interrupt 8
                        .dw int_vector_unhandled        ; External interrupt 9
                        .dw int_vector_unhandled        ; External interrupt a
                        .dw int_vector_unhandled        ; External interrupt b
                        .dw int_vector_c_timer          ; External interrupt c
                        .dw int_vector_unhandled        ; External interrupt d
                        .dw int_vector_unhandled        ; External interrupt e
                        .dw int_vector_unhandled        ; External interrupt f
                        .dw reset_vector                ; Processor reset

                        .end
                        