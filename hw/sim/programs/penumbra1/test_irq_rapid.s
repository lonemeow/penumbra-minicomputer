; test_irq_rapid.s — Forward-progress under rapid back-to-back timer IRQs
;
; Sequence under test:
;   1. Configure the timer with reload=1 (fastest possible — fires every
;      ~13 CPU cycles at 25 MHz with the 1 MHz tick prescaler).
;   2. Install a handler that acks the timer, increments a counter, and
;      either ERETs (counter < TARGET) or self-terminates (counter ==
;      TARGET) by setting R1=1 and BREAKing from inside the handler.
;   3. User code is a trivial busy-wait — it never needs to make
;      progress for the test to succeed.  All forward progress is
;      driven by the handler.
;
; Each int_entry → handler → ERET → pending-IRQ → int_entry cycle
; should advance the counter by exactly one.  After TARGET iterations,
; the handler BREAKs.
;
; Failure modes this test catches:
;   - Deadlock between int_entry and pending-IRQ logic: counter never
;     reaches TARGET → cycle limit hit → FAIL
;   - ERET-into-pending-IRQ broken: kernel returns to user but IRQ
;     never re-fires → user spin never preempted → counter stuck →
;     cycle limit hit
;   - Corrupted EPC across rapid IRQs: ERET jumps somewhere unexpected
;     → either crashes or hits a different BREAK
;   - int_entry preempted by pending IRQ: would either lose increments
;     (counter takes longer to hit target — still passes within budget)
;     or corrupt state (FAIL via cycle limit / wrong PC)
;
; This is the most direct stress test for "ERET into pending IRQ"
; corner cases.  TARGET=30 iterations × ~25 cycles each ≈ 750 CPU
; cycles plus setup; cycle limit is 500000.
;
; Result: R1=1 PASS, R1=0 FAIL (timeout)

.equ COUNTER_ADDR, 0x100
.equ TARGET_COUNT, 30

_start:
    LLI   R1, #0                  ; assume fail

    ; ── Install timer handler ──────────────────────────────
    LA    R2, #timer_handler
    STW   R2, [R0 + #4]           ; vector[1] = VEC_TIMER

    ; ── Clear counter ──────────────────────────────────────
    STW   R0, [R0 + #COUNTER_ADDR]

    ; ── Configure timer: reload=1, IRQ enabled, auto-load ──
    LLI   R3, #1
    WRSYS R3, #TIMER, #TM_RELOAD
    WRSYS R3, #TIMER, #TM_COUNT
    LLI   R3, #0x07               ; TICK_EN | IRQ_EN | AUTOLOAD
    WRSYS R3, #TIMER, #TM_CR

    EI
    NOP                            ; ei_shadow consumed

    ; ── Trivial spin — handler drives all forward progress ──
forever:
    NOP
    B     forever

; ═══════════════════════════════════════════════════════════════
; Timer handler — ack, increment, self-terminate at target
; ═══════════════════════════════════════════════════════════════
timer_handler:
    ; Clear underflow flag (write-1-to-clear).
    LLI   R10, #1
    WRSYS R10, #TIMER, #TM_STATUS

    ; Increment counter
    LDW   R11, [R0 + #COUNTER_ADDR]
    ADD   R11, #1
    STW   R11, [R0 + #COUNTER_ADDR]

    ; Check target
    CMP   R11, #TARGET_COUNT
    BHS   pass

    ERET

pass:
    ; Disable the timer cleanly so we don't fire again before BREAK
    ; commits.  (BREAK preempts IRQ at dispatch — see priority order
    ; in cpu_core.sv — but defense-in-depth.)
    LLI   R3, #0
    WRSYS R3, #TIMER, #TM_CR

    LLI   R1, #1
    BREAK
