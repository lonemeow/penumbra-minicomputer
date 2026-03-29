; boot_rom.s — Penumbra/1 Boot ROM
;
; Identical binary runs on simulator (tb_interactive) and real hardware.
; Prints a banner over UART, then enters an echo loop for interactive use.
;
; Register conventions (set once, used by all I/O routines):
;   R12 = UART base address (0xFF00_0000)
;   R11 = LSR_THRE mask (0x20) — used by putchar
;   R10 = LSR_DR mask (0x01)   — used by getchar
;   R8  = saved link register  — used by puts (nested BL)

.equ UART, 0xFF000000

_start:
    ; ── Set up UART register constants ──────────────────────
    LI   R12, #UART
    LLI  R11, #LSR_THRE
    LLI  R10, #LSR_DR

    ; ── Print banner ────────────────────────────────────────
    LA   R4, #banner
    BL   puts

    ; ── Echo loop ───────────────────────────────────────────
    ; Read a character from UART, echo it back, repeat forever.
echo_loop:
    BL   getchar
    BL   putchar
    B    echo_loop

; ═══════════════════════════════════════════════════════════════
; I/O routines
; ═══════════════════════════════════════════════════════════════

; ── putchar ─────────────────────────────────────────────────
; Send one character over UART (poll until TX ready).
; Input:  R2 = character (low 8 bits)
; Uses:   R11 = LSR_THRE mask, R12 = UART base
; Clobbers: R3
putchar:
    LDW  R3, [R12 + #UART_LSR]
    TEST R3, R11
    BZ   putchar
    STW  R2, [R12 + #UART_DATA]
    RET

; ── getchar ─────────────────────────────────────────────────
; Read one character from UART (poll until RX data ready).
; Output: R2 = received character (zero-extended byte)
; Uses:   R10 = LSR_DR mask, R12 = UART base
; Clobbers: R3
getchar:
    LDW  R3, [R12 + #UART_LSR]
    TEST R3, R10
    BZ   getchar
    LDW  R2, [R12 + #UART_DATA]
    RET

; ── puts ────────────────────────────────────────────────────
; Print a null-terminated string over UART.
; Input:  R4 = pointer to string
; Uses:   putchar, R8 = saved LR
; Clobbers: R2, R3, R4, R8
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
; Data
; ═══════════════════════════════════════════════════════════════

banner:
    .asciz "Penumbra/1\n> "
