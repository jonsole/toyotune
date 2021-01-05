                        .MODULE sde
                        .LOCALLABELCHAR "."
                        
serial_diag_extended:                
                        
; Flush any stale data in UART receive buffer
.diag_flush:
                        ld      a, SIDR_SODR
                        tbbs    bit7, SSD, .diag_flush          ; Loop back if SSD.7 set (data in receive buffer)
                        
; Send sync byte                        
                        clrb    bit1, SSD                       ; Clear SSD.1 which sets parity bit to space (DIAG)
                        ld      a, serial_diag_mode             ; Copy mode value...
                        st      a, SIDR_SODR                    ; ...to serial Tx buffer

                        ld      y, #.diag_mode_table            ; Load table base for diag mode table
                        sub     a, #0DAh                        ; Convert mode into index and...
                        shl     a                               ; ...multiply by 2
                        add     y, a                            ; Compute address in table
                        ld      y, y + 00h                      ; Load jump address into y for selected mode

                        ld      b, #10h                         ; Initialise loop counter (16 iterations)

; Wait for MSB of command/address to be received
.diag_msb_wait:
                        tbbs    bit7, SSD, .diag_msb_found      ; Exit loop if SSD.7 set (data in receive buffer)
                        dec     b                               ; Repeat for 14 iterations...
                        bne     .diag_msb_wait                  ; ...loop back if more iterations
                        bra     .diag_exit                      ; Timed out waiting for data

; MSB of command/address received
.diag_msb_found:                                              
                        tbbs    bit6, SSD, .diag_drop           ; Jump if SSD.6 (buffer overflow) set
                        ld      a, SIDR_SODR                    ; Store MSB in A

; Wait for LSB of command/address to be received                        
.diag_lsb_wait:
                        tbbs    bit7, SSD, .diag_lsb_found      ; Exit loop if SSD.7 set (data in receive buffer)
                        dec     b                               ; Repeat for 14 iterations...
                        bne     .diag_lsb_wait                  ; ...loop back if more iterations
                        bra     .diag_exit                      ; Timed out waiting for data

; LSB of command/address received
.diag_lsb_found:        tbbs    bit6, SSD, .diag_drop           ; Jump if SSD.6 (buffer overflow) set
                        ld      b, SIDR_SODR                    ; Store LSB in B, 16 bit value now in D

                        tbbs    bit0, SSD, .diag_command        ; Read Serial Status Register, jump if parity bit is set (command selection bit)
                        jmp     y + 00h                         ; Not command, so jump to mode handler routine

; Handler for read 16 mode
.diag_data_read16_mode:
                        mov     d, y                            ; Move address to Y
                        ld      d, [y]                          ; Read 2 bytes and increment address                        

; Transmit word in D
.diag_tx_word:
                        clrb    bit1, SSD                       ; Clear SSD.1 which sets parity bit to space (DIAG)
                        st      a, SIDR_SODR                    ; Write 1st byte to serial Tx buffer
                        div     d, #00h                         ; Wait for a bit...                        
                        clrb    bit1, SSD                       ; Clear SSD.1 which sets parity bit to space (DIAG)
                        st      b, SIDR_SODR                    ; Write 2nd byte to serial Tx buffer
                        
.diag_exit:
                        clrb    bit3, IRQLL
                        ret

; Handler for write 16 bits mode, address phase
.diag_addr_write16_mode:
                        st      d, serial_diag_address          ; Store write address
                        ld      #0DCh, serial_diag_mode         ; Move to data_write16 mode
                        bra     .diag_tx_word                   ; Echo back address
                        
; Handler for write 16 bits mode, data phase
.diag_data_write16_mode:
                        ld      y, serial_diag_address          ; Retrieve write address
                        st      d, [y]                          ; Write 16 bit value in D, increment write address
                        st      y, serial_diag_address          ; Store write address
                        bra     .diag_tx_word                   ; Echo back address
                        
; Handler for write 8 bits mode, address phase
.diag_addr_write8_mode:
                        st      d, serial_diag_address          ; Store write address
                        ld      #0DEh, serial_diag_mode         ; Move to data_write8 mode    
                        bra     .diag_tx_word                   ; Echo back address
                        
; Handler for write 16 bits mode, data phase
.diag_data_write8_mode:
                        ld      y, serial_diag_address          ; Retrieve write address
                        st      a, [y]                          ; Write 8 bit value in A, increment write address
                        st      y, serial_diag_address          ; Store write address
                        bra     .diag_tx_word                   ; Echo back address

; Command received, store and echo back              
.diag_command:
                        cmp     b, #0DAh                        ; Check if command < 0DAh...
                        bcs     .diag_drop                      ; Jump if it is, command is invalid
                        cmp     b, #0DEh                        ; Check if command > 0DEh
                        bgt     .diag_drop                      ; Jump if it is, command is invalid                                
                        st      b, serial_diag_mode             ; Store new diagnostic mode                        
                        bra     .diag_tx_word                   ; Echo back command  
                        
; Drop any data in buffer                        
.diag_drop:
                        ld      a, SIDR_SODR
                        tbbs    bit7, SSD, .diag_drop            ; Loop back if SSD.7 set (data in receive buffer)
                        bra     .diag_exit

.diag_mode_table:
                        .dw     .diag_data_read16_mode          ; 0DAh
                        .dw     .diag_addr_write16_mode         ; 0DBh
                        .dw     .diag_data_write16_mode         ; 0DCh
                        .dw     .diag_addr_write8_mode          ; 0DDh
                        .dw     .diag_data_write8_mode          ; 0DEh
