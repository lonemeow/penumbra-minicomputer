; test_uart.s — Verify 16450-compatible UART registers and TX output
; REQUIRES: uart
;
; Tests:
;   1. SCR round-trip (write/read scratch register)
;   2. LSR shows TX ready on reset (THRE + TEMT bits)
;   3. DLAB mode: write/read DLL/DLM baud divisor
;   4. IIR identifies as 16450 (no FIFO, bits 7:6 = 00)
;   5. TX output: write a char, verify THRE goes low, poll until ready
;   6. uart_putchar: poll-based character output routine prints "OK\n"
;
; Result: R1=1 PASS, R1=0 FAIL

.equ UART,  0xFF000000

_start:
    LLI  R1, #0               ; assume fail

    ; ── Load UART base address into R12 (used throughout) ────
    LI   R12, #UART

    ; ── Prepare mask registers ───────────────────────────────
    ; R10 = 0xFF (byte mask), R11 = LSR_THRE (0x20)
    LLI  R10, #0xFF
    LLI  R11, #LSR_THRE

    ; ── Test 1: SCR round-trip ───────────────────────────────
    ; Scratch register should read back what we write.
    ; This is how probe routines detect a real UART.
    LLI  R2, #0xA5
    STW  R2, [R12 + #UART_SCR]
    LDW  R3, [R12 + #UART_SCR]
    AND  R3, R10               ; mask to byte (word read)
    CMP  R3, #0xA5
    BNE  fail

    ; Write a second pattern to confirm it's not stuck
    LLI  R2, #0x5A
    STW  R2, [R12 + #UART_SCR]
    LDW  R3, [R12 + #UART_SCR]
    AND  R3, R10
    CMP  R3, #0x5A
    BNE  fail

    ; ── Test 2: LSR shows TX ready ───────────────────────────
    ; On reset, THRE (bit 5) and TEMT (bit 6) should be set.
    LLI  R9, #0x60            ; THRE + TEMT mask
    LDW  R3, [R12 + #UART_LSR]
    AND  R3, R9
    CMP  R3, #0x60
    BNE  fail

    ; ── Test 3: DLAB mode — baud divisor round-trip ──────────
    ; Set DLAB=1, write DLL/DLM, read back, clear DLAB
    LLI  R2, #0x80            ; LCR with DLAB=1
    STW  R2, [R12 + #UART_LCR]

    LLI  R2, #0x03            ; DLL = 3 (low byte of divisor)
    STW  R2, [R12 + #UART_DLL]
    LLI  R2, #0x00            ; DLM = 0 (high byte)
    STW  R2, [R12 + #UART_DLM]

    LDW  R3, [R12 + #UART_DLL]
    AND  R3, R10
    CMP  R3, #0x03
    BNE  fail

    ; Clear DLAB
    LLI  R2, #0x00
    STW  R2, [R12 + #UART_LCR]

    ; ── Test 4: IIR identifies as 16450 ──────────────────────
    ; Bits 7:6 should be 00 (no FIFO), bit 0 should be 1 (no IRQ pending)
    LDW  R3, [R12 + #UART_IIR]
    AND  R3, R10
    CMP  R3, #0x01             ; no interrupt, no FIFO
    BNE  fail

    ; ── Test 5: TX a character ───────────────────────────────
    ; Write 'P' to THR, verify THRE goes low (TX busy), then
    ; poll until it comes back (TX_BUSY_CYCLES elapsed).
    LLI  R2, #0x50            ; 'P'
    STW  R2, [R12 + #UART_DATA]

    ; THRE should be low immediately after write (TX in progress)
    LDW  R3, [R12 + #UART_LSR]
    AND  R3, R11               ; mask to THRE bit
    CMP  R3, #0
    BNE  fail                  ; fail if THRE is already set

    ; Poll until THRE comes back
test5_poll:
    LDW  R3, [R12 + #UART_LSR]
    TEST R3, R11
    BZ   test5_poll

    ; ── Test 6: uart_putchar routine ─────────────────────────
    ; Use the polling routine to send a full string "OK\n"
    LLI  R2, #0x4F            ; 'O'
    BL   uart_putchar
    LLI  R2, #0x4B            ; 'K'
    BL   uart_putchar
    LLI  R2, #0x0A            ; '\n'
    BL   uart_putchar

    ; All tests passed
    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; uart_putchar — Poll LSR and transmit one character
; ═══════════════════════════════════════════════════════════════
; Input:  R2  = character to send (low 8 bits)
;         R11 = LSR_THRE mask (0x20)
;         R12 = UART base address
; Clobbers: R3
uart_putchar:
    LDW  R3, [R12 + #UART_LSR]
    TEST R3, R11
    BZ   uart_putchar
    STW  R2, [R12 + #UART_DATA]
    RET
