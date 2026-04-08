; test_timer.s — Timer interrupt fires and handler runs
;
; Setup:
;   - Install timer interrupt handler at vector 1 (VEC_TIMER)
;   - Configure timer: reload=2, auto-reload, IRQ enabled
;   - Enable interrupts (EI)
;   - Spin in a loop until the handler has fired at least 3 times
;   - Handler increments a counter in RAM at address 0x100
;
; The timer ticks at 1 MHz (prescaler ÷25 from 25 MHz CPU clock).
; With reload=2, the period is 3 timer ticks = 75 CPU cycles per
; interrupt (plus synchronizer latency). Three interrupts should
; arrive within a few hundred CPU cycles.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ COUNTER_ADDR, 0x100
.equ TARGET_COUNT, 3
.equ TIMEOUT, 10000          ; loop iterations before giving up

_start:
    LLI  R1, #0               ; assume fail

    ; ── Install vector table ─────────────────────────────────
    ; Vector 1 = timer interrupt (VEC_TIMER)
    LA   R2, #timer_handler
    STW  R2, [R0 + #4]        ; vector[1] at physical 0x04

    ; ── Clear interrupt counter in RAM ───────────────────────
    STW  R0, [R0 + #COUNTER_ADDR]

    ; ── Configure timer ──────────────────────────────────────
    ; Set reload value (small for fast test)
    LLI  R3, #2
    WRSYS R3, #TIMER, #TM_RELOAD

    ; Set initial count
    WRSYS R3, #TIMER, #TM_COUNT

    ; Enable: TICK_EN | IRQ_EN | AUTOLOAD = 0x07
    LLI  R3, #0x07
    WRSYS R3, #TIMER, #TM_CR

    ; ── Enable CPU interrupts ────────────────────────────────
    EI

    ; ── Spin until counter reaches target ────────────────────
    LLI  R4, #TARGET_COUNT
    LLI  R5, #TIMEOUT
    LLI  R8, #1               ; decrement constant
spin:
    LDW  R7, [R0 + #COUNTER_ADDR]
    CMP  R7, R4
    BHS  pass                 ; counter >= TARGET_COUNT
    NOP
    NOP
    NOP
    NOP
    SUB  R5, R8               ; timeout--  (R8=1, set below)
    BNE  spin                 ; not timed out yet

    ; Timeout — FAIL
    BREAK

pass:
    ; ── Disable timer ────────────────────────────────────────
    LLI  R3, #0
    WRSYS R3, #TIMER, #TM_CR
    DI

    ; PASS
    LLI  R1, #1
    BREAK

; ═══════════════════════════════════════════════════════════════
; Timer interrupt handler
; ═══════════════════════════════════════════════════════════════
timer_handler:
    ; Increment counter at COUNTER_ADDR
    LDW  R9, [R0 + #COUNTER_ADDR]
    ADD  R9, #1
    STW  R9, [R0 + #COUNTER_ADDR]

    ; Clear UDF flag (write-1-to-clear TMSTATUS[0])
    LLI  R10, #1
    WRSYS R10, #TIMER, #TM_STATUS

    ; Return from interrupt
    ERET
