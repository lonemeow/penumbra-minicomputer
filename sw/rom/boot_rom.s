; boot_rom.s — Penumbra/1 Boot ROM
;
; Identical binary runs on simulator (tb_interactive) and real hardware.
; Prints banner, accepts monitor commands over UART.
;
; Commands:
;   d ADDR       — dump 64 bytes of memory
;   w ADDR VAL   — write 32-bit word to memory
;   g ADDR       — jump to address (no return)
;   ?            — help
;
; Calling convention:
;   R14 (SP)     — stack pointer, grows downward
;   R13 (LR)     — link register, saved to stack by non-leaf functions
;   R10-R12      — global I/O registers (never clobbered)
;   R2-R9        — scratch / arguments (may be clobbered by any call)
;
; Key registers:
;   R12 = UART base (0xFF00_0000)
;   R11 = LSR_THRE mask (0x20)
;   R10 = LSR_DR mask (0x01)
;
; Argument/result registers:
;   R2  = character I/O (putchar/getchar)
;   R4  = string/buffer pointer (puts, readline, parse_hex, skip_spaces)
;   R5  = print_hex input value
;   R6  = parse_hex output value

.equ UART, 0xFF000000
.equ STACK_TOP, 0x01000000
.equ LINE_BUF, 0x00001000
.equ LINE_MAX, 79

; ═══════════════════════════════════════════════════════════════
; Entry point
; ═══════════════════════════════════════════════════════════════

_start:
    LI   R14, #STACK_TOP
    LI   R12, #UART
    LLI  R11, #LSR_THRE
    LLI  R10, #LSR_DR

    LA   R4, #banner
    BL   puts

; ═══════════════════════════════════════════════════════════════
; Main command loop
; ═══════════════════════════════════════════════════════════════

main_loop:
    ; Print prompt
    LLI  R2, #0x3E            ; '>'
    BL   putchar
    LLI  R2, #0x20            ; ' '
    BL   putchar

    ; Read line
    BL   readline             ; R4 = buffer start, R5 = length

    ; Empty line → re-prompt
    CMP  R5, #0
    BEQ  main_loop

    ; Get command character and advance past it
    LDB  R6, [R4 + #0]
    ADD  R4, #1

    ; ── Dispatch (case-insensitive) ─────────────────────────
    CMP  R6, #0x64            ; 'd'
    BEQ  cmd_dump
    CMP  R6, #0x44            ; 'D'
    BEQ  cmd_dump
    CMP  R6, #0x77            ; 'w'
    BEQ  cmd_write
    CMP  R6, #0x57            ; 'W'
    BEQ  cmd_write
    CMP  R6, #0x67            ; 'g'
    BEQ  cmd_go
    CMP  R6, #0x47            ; 'G'
    BEQ  cmd_go
    CMP  R6, #0x3F            ; '?'
    BEQ  cmd_help

    ; Unknown command
    LA   R4, #msg_unknown
    BL   puts
    B    main_loop

; ═══════════════════════════════════════════════════════════════
; Command handlers
; ═══════════════════════════════════════════════════════════════

; ── d ADDR — dump 64 bytes (4 lines × 16 bytes) ────────────
cmd_dump:
    SUB  R14, #4
    STW  R13, [R14 + #0]
    BL   skip_spaces
    BL   parse_hex            ; R6 = start address
    LLI  R8, #4               ; 4 lines
cmd_dump_line:
    ; Print address
    MOV  R5, R6
    BL   print_hex32
    LLI  R2, #0x20
    BL   putchar
    BL   putchar
    ; Print 16 bytes
    LLI  R9, #16
cmd_dump_byte:
    LDB  R5, [R6 + #0]
    BL   print_hex8
    LLI  R2, #0x20
    BL   putchar
    ADD  R6, #1
    SUB  R9, #1
    BNZ  cmd_dump_byte
    ; End of line
    BL   newline
    SUB  R8, #1
    BNZ  cmd_dump_line
    LDW  R13, [R14 + #0]
    ADD  R14, #4
    B    main_loop

; ── w ADDR VAL — write 32-bit word ─────────────────────────
cmd_write:
    SUB  R14, #4
    STW  R13, [R14 + #0]
    BL   skip_spaces
    BL   parse_hex            ; R6 = address
    MOV  R8, R6               ; save address
    BL   skip_spaces
    BL   parse_hex            ; R6 = value
    STW  R6, [R8 + #0]
    ; Confirm
    LA   R4, #msg_ok
    BL   puts
    LDW  R13, [R14 + #0]
    ADD  R14, #4
    B    main_loop

; ── g ADDR — jump to address ───────────────────────────────
cmd_go:
    BL   skip_spaces
    BL   parse_hex            ; R6 = address
    JMP  R6

; ── ? — print help ─────────────────────────────────────────
cmd_help:
    LA   R4, #msg_help
    BL   puts
    B    main_loop

; ═══════════════════════════════════════════════════════════════
; I/O routines
; ═══════════════════════════════════════════════════════════════

; ── putchar ─────────────────────────────────────────────────
; Input: R2 = character. Clobbers: R3.
putchar:
    LDW  R3, [R12 + #UART_LSR]
    TEST R3, R11
    BZ   putchar
    STW  R2, [R12 + #UART_DATA]
    RET

; ── getchar ─────────────────────────────────────────────────
; Output: R2 = character. Clobbers: R3.
getchar:
    LDW  R3, [R12 + #UART_LSR]
    TEST R3, R10
    BZ   getchar
    LDW  R2, [R12 + #UART_DATA]
    RET

; ── newline ─────────────────────────────────────────────────
; Print CR+LF. Clobbers: R2, R3.
newline:
    SUB  R14, #4
    STW  R13, [R14 + #0]
    LLI  R2, #0x0D
    BL   putchar
    LLI  R2, #0x0A
    BL   putchar
    LDW  R13, [R14 + #0]
    ADD  R14, #4
    RET

; ── puts ────────────────────────────────────────────────────
; Input: R4 = pointer to null-terminated string.
; Clobbers: R2, R3, R4, R8.
puts:
    MOV  R8, R13
puts_loop:
    LDB  R2, [R4 + #0]
    CMP  R2, #0
    BEQ  puts_done
    BL   putchar
    ADD  R4, #1
    B    puts_loop
puts_done:
    JMP  R8

; ═══════════════════════════════════════════════════════════════
; Line editing
; ═══════════════════════════════════════════════════════════════

; ── readline ────────────────────────────────────────────────
; Read a line from UART into LINE_BUF with echo and backspace.
; Output: R4 = buffer start, R5 = character count.
; Clobbers: R2, R3.
readline:
    SUB  R14, #4
    STW  R13, [R14 + #0]
    LI   R4, #LINE_BUF
    LLI  R5, #0
readline_loop:
    BL   getchar
    ; CR or LF → done
    CMP  R2, #0x0D
    BEQ  readline_done
    CMP  R2, #0x0A
    BEQ  readline_done
    ; Backspace (BS or DEL)
    CMP  R2, #0x08
    BEQ  readline_bs
    CMP  R2, #0x7F
    BEQ  readline_bs
    ; Ignore non-printable
    CMP  R2, #0x20
    BLO  readline_loop
    ; Buffer full?
    CMP  R5, #LINE_MAX
    BHS  readline_loop
    ; Store and echo
    STB  R2, [R4 + #0]
    ADD  R4, #1
    ADD  R5, #1
    BL   putchar
    B    readline_loop
readline_bs:
    CMP  R5, #0
    BEQ  readline_loop
    SUB  R4, #1
    SUB  R5, #1
    ; Erase on terminal: BS, space, BS
    LLI  R2, #0x08
    BL   putchar
    LLI  R2, #0x20
    BL   putchar
    LLI  R2, #0x08
    BL   putchar
    B    readline_loop
readline_done:
    ; Null-terminate
    LLI  R2, #0
    STB  R2, [R4 + #0]
    BL   newline
    ; Restore R4 to buffer start
    SUB  R4, R5
    LDW  R13, [R14 + #0]
    ADD  R14, #4
    RET

; ═══════════════════════════════════════════════════════════════
; Hex conversion
; ═══════════════════════════════════════════════════════════════

; ── skip_spaces ─────────────────────────────────────────────
; Advance R4 past ASCII spaces. Clobbers: R2.
skip_spaces:
    LDB  R2, [R4 + #0]
    CMP  R2, #0x20
    BNE  skip_spaces_done
    ADD  R4, #1
    B    skip_spaces
skip_spaces_done:
    RET

; ── parse_hex ───────────────────────────────────────────────
; Parse hex string at R4 into R6. Advances R4 past digits.
; Output: R6 = parsed value. Clobbers: R2, R3, R7.
parse_hex:
    LLI  R6, #0
parse_hex_loop:
    LDB  R2, [R4 + #0]
    ; Classify: '0'-'9'
    CMP  R2, #0x30
    BLO  parse_hex_done
    CMP  R2, #0x3A
    BLO  parse_hex_09
    ; Classify: 'A'-'F'
    CMP  R2, #0x41
    BLO  parse_hex_done
    CMP  R2, #0x47
    BLO  parse_hex_AF
    ; Classify: 'a'-'f'
    CMP  R2, #0x61
    BLO  parse_hex_done
    CMP  R2, #0x67
    BLO  parse_hex_af
    B    parse_hex_done
parse_hex_09:
    MOV  R3, R2
    SUB  R3, #0x30            ; digit = char - '0'
    B    parse_hex_got
parse_hex_AF:
    MOV  R3, R2
    SUB  R3, #0x37            ; digit = char - 'A' + 10
    B    parse_hex_got
parse_hex_af:
    MOV  R3, R2
    SUB  R3, #0x57            ; digit = char - 'a' + 10
parse_hex_got:
    ; Accumulate: R6 = (R6 << 4) | digit
    LLI  R7, #4
    SHL  R6, R7
    OR   R6, R3
    ADD  R4, #1
    B    parse_hex_loop
parse_hex_done:
    RET

; ── print_hex32 ─────────────────────────────────────────────
; Print R5 as 8 hex digits. Preserves R5.
; Clobbers: R2, R3, R7.
print_hex32:
    SUB  R14, #12
    STW  R13, [R14 + #8]
    STW  R5, [R14 + #4]
    STW  R7, [R14 + #0]
    LLI  R7, #8
print_hex32_loop:
    ; Extract top nibble
    MOV  R2, R5
    LLI  R3, #28
    SHR  R2, R3
    BL   print_nibble
    ; Shift to next nibble
    LLI  R3, #4
    SHL  R5, R3
    SUB  R7, #1
    BNZ  print_hex32_loop
    LDW  R7, [R14 + #0]
    LDW  R5, [R14 + #4]
    LDW  R13, [R14 + #8]
    ADD  R14, #12
    RET

; ── print_hex8 ──────────────────────────────────────────────
; Print low byte of R5 as 2 hex digits. Preserves R5.
; Clobbers: R2, R3.
print_hex8:
    SUB  R14, #8
    STW  R13, [R14 + #4]
    STW  R5, [R14 + #0]
    ; High nibble
    MOV  R2, R5
    LLI  R3, #4
    SHR  R2, R3
    LLI  R3, #0x0F
    AND  R2, R3
    BL   print_nibble
    ; Low nibble
    MOV  R2, R5
    LLI  R3, #0x0F
    AND  R2, R3
    BL   print_nibble
    LDW  R5, [R14 + #0]
    LDW  R13, [R14 + #4]
    ADD  R14, #8
    RET

; ── print_nibble ────────────────────────────────────────────
; Print R2 (0-15) as a single hex character.
; Input:  R2 = nibble value (0-15)
; Output: character printed via putchar
; Clobbers: R2, R3 (via putchar)
print_nibble:
    LA   R3, hex_chars
    ADD  R3, R2
    LDB  R2, [R3]
    B    putchar              ; tail call — putchar's RET returns to our caller

; ═══════════════════════════════════════════════════════════════
; Data
; ═══════════════════════════════════════════════════════════════

banner:
    .asciz "Penumbra/1\r\n"

msg_help:
    .asciz "d ADDR     dump 64 bytes\r\nw ADDR VAL write word\r\ng ADDR     go (jump)\r\n?          help\r\n"

msg_unknown:
    .asciz "?\r\n"

msg_ok:
    .asciz "OK\r\n"

hex_chars:
    .asciz "0123456789ABCDEF"
