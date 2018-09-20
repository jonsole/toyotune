                        .MODULE sde
                        .LOCALLABELCHAR "."
                        
serial_diag_extended:                
                        
; Flush any stale data in UART receive buffer
.data_flush:
                        ld      a, SIDR_SODR
                        tbbs    bit7, SSD, .data_flush          ; Loop back if SSD.7 set (data in receive buffer)
                        
; Send 0xDA sync byte                        
                        clrb    bit1, SSD                       ; Clear SSD.1 which sets parity bit to space (DIAG)
                        ld      a, #0DAh                        ; Write 0xDA...
                        st      a, SIDR_SODR                    ; ...to serial Tx buffer

; Load Y with address of handler function
                        ld      a, serial_diag_mode             ; Load diagnostic mode
                        and     a, #0FEh                        ; Mask out status bit
                        ld      y, #.diag_mode_table            ; Load table base for channel jump address
                        add     y, a                            ; Compute table index
                        ld      y, y + 00h                      ; Load jump address into y for selected mode

; Wait for command/address to be received
                        ld      a, #0Eh                         ; Initialise loop counter (14 iterations)
.data_wait:
                        tbbs    bit7, SSD, .data_found          ; Exit loop if SSD.7 set (data in receive buffer)
                        dec     a                               ; Repeat for 14 iterations...
                        bne     .data_wait                      ; ...loop back if more iterations
                        bra     .data_drop                      ; Timed out waiting for data

; Command/address received
.data_found:                                              
                        tbbc    bit6, SSD, .data_ok             ; Jump if SSD.6 (what's this?) cleared

; Drop received data
.data_drop:
                        ld      a, SIDR_SODR                    ; Read data from serial receive buffer

; Reset diagnostic mode back to normal
.diag_mode_reset:
                        clr     b
                        st      b, serial_diag_mode             ; Reset diagnostic mode back to normal
                        bra     .diag_return                    ; Jump to end, discard serial data

; Received command/address successfull, pass to mode handler routine
.data_ok:                                                       
                        ld      b, SIDR_SODR                    ; Read data from serial receive buffer
                        ld      a, SSD                          ; Read Serial Status Register...
                        and     a, #01h                         ; ...bottom bit is parity bit and bit 8 of requested address
                        jmp     y + 00h                         ; Jump to mode handler routine

; Table of mode handler routine addresses
.diag_mode_table:
                        .dw     .diag_normal                    ; Mode 0 (normal diagnostics)
                        .dw     .diag_addr_msb                  ; Mode 1 (RW Address MSB)
                        .dw     .diag_addr_lsb                  ; Mode 2 (RW Address LSB)
                        .dw     .diag_write                     ; Mode 3 (RW Data)

; Normnal mode handler, check for special commands                                                
.diag_normal:
                        cmp     d, #001Fh                       ; Check if address matches request for SW version number...
                        beq     .diag_sw_ver                    ; ...jump to routine if so
                        cmp     d, #001Eh                       ; Check if address matched request for read next word
                        beq     .diag_read_next                 ; ...jump to routine if so
                        cmp     d, #0019h
                        beq     .diag_set_mode_1                ; Branch if address is 32h (Mode 1)
                        cmp     d, #001Ah
                        beq     .diag_set_mode_3                ; Branch if address is 34h (Continuous write)

                        shl     d                               ; Multiply D by 2 to get 16 bit word address in RAM 
                        mov     d, y                            ; Copy address of bytes in RAM to Y
                        ld      d, [y]                          ; Read 2 bytes and increment address                        

; Get cache next word addressed by Y
.data_tx_word_next:
                        st      y, serial_diag_address          ; Store address of next word                        
                        ld      y, y + 0                        ; Get next 2 bytes
                        st      y, serial_diag_data             ; ...and store it in buffer to allow atomic reads of 32 bit values

; Transmit word in D
.data_tx_word:
                        clrb    bit1, SSD                       ; Clear SSD.1 which sets parity bit to space (DIAG)
                        st      a, SIDR_SODR                    ; Write 1st byte to serial Tx buffer
                        div     d, #00h                         ; Wait for a bit...
                        
; Transmit byte in B
.data_tx_byte:
                        clrb    bit1, SSD                       ; Clear SSD.1 which sets parity bit to space (DIAG)
                        st      b, SIDR_SODR                    ; Write 2nd byte to serial Tx buffer
                        
; Clear interrupt request and return                       
.diag_return:
                        clrb    bit3, IRQLL
                        ret
                      
; Read SW version: Echo back 16 bit SW version
.diag_sw_ver:
                        ld      d, rom_version                  ; Load D with SW version 16 bit word
                        bra     .data_tx_word                   ; Transmit version back

; Read next word: Echo back 16 bit value following previous read
.diag_read_next:
                        ld      d, serial_diag_data             ; Load X with value cached following previous read
                        bra     .data_tx_word                   ; Transmit value back
                        
; Set Mode 1: Echo back command byte and wait for address MSB in next iteration
.diag_set_mode_1:  
                        ld      a, #(1 << 1)
                        st      a, serial_diag_mode             ; Set diagnostic mode 1
                        bra     .data_tx_byte                   ; Transmit command back
                        
; Set Mode 2: Address MSB in Rx buffer
.diag_addr_msb:
                        st      b, serial_diag_address          ; Store write address MSB
                        ld      a, #(2 << 1)
                        st      a, serial_diag_mode             ; Set diagnostics mode 2
                        bra     .data_tx_byte                   ; Transmit command back
                        
; Set Mode 3: Address LSB in Rx buffer
.diag_addr_lsb:
                        st      b, serial_diag_address + 1      ; Store write address LSB
                
; Continue block data write using the last computed address
.diag_set_mode_3:
                        ld      a, #(3 << 1)
                        st      a, serial_diag_mode             ; Set diagnostics mode 3
                        bra     .data_tx_byte                   ; Transmit command back

; ---------------------------------------------------------------------------
; if data sent has bit 9 set, then it is a last data byte, so reset back to Mode 0
; if RAM_Stat==1, then is on word boundary so write word,
; else (RAM_Stat==0) write byte if bit9 set, save byte if bit9 clr

.diag_write:
                        ld      y, serial_diag_address          ; Load the address pointer
 
                        shr     a                               ; Check if bit 8 set to indicate last byte of write...
                        bcs     .diag_data_last                 ; ...and jump
                                                
                        ld      a, serial_diag_mode             ; Load diagnostic mode and LSB/MSB status
                        xor     a, #01h                         ; Toggle status
                        st      a, serial_diag_mode             ; Set new LSB/MSB status
                        shr     a                        
                        bcs     .diag_write_msb                 ; ...jump if MSB byte

; LSB, write both MSB & LSB to RAM
.diag_write_lsb:        ld      a, serial_diag_data             ; Retrieve MSB of data
                        st      d, [y]                          ; Word write, autoincrement address in Y
                        st      y, serial_diag_address          ; Save incremented address
                        bra     .data_tx_byte                   ; Transmit byte back

; MSB, store MSB in buffer                      
.diag_write_msb:
                        st      b, serial_diag_data             ; Save MSB data
                        bra     .data_tx_byte                   ; Transmit byte back
                        
; Final write
.diag_data_last:
                        ld      a, serial_diag_mode             ; Load diagnostic mode and LSB/MSB status 
                        and     a, #01h                         ; Reset diagnostic mode back to normal
                        shr     a                               ; Shift into carry flag
                        st      a, serial_diag_mode             ; Set diagnostic mode
                        bcs     .diag_write_lsb                 ; Jump if final write is LSB byte

; Final byte is MSB, write MSB only
                        xch     a, b                            ; Move LSB into A
                        st      a, [y]                          ; Byte write, autoincrement address in Y
                        st      y, serial_diag_address          ; Save incremented address
                        bra     .data_tx_byte                   ; Transmit byte back
                        
