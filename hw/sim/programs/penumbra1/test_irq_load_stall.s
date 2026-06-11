; test_irq_load_stall.s — Loads under timer IRQ pressure
;
; Sequence under test:
;   1. Pre-populate a known data pattern in RAM.
;   2. Configure the timer to fire repeatedly (reload=3 → period ~80
;      CPU cycles).
;   3. Run a tight loop that loads each pattern word, sums it into
;      an accumulator, and compares against the expected per-iter
;      value.  Any mismatch fails the test immediately.
;   4. The timer fires asynchronously throughout.  Each load could
;      potentially be in STALL when an IRQ goes pending.
;
; Failure modes this test catches:
;   - Load corrupted by IRQ-induced STALL abort (mismatch → FAIL)
;   - Load committed for the wrong destination register (mismatch)
;   - ESR/EPC captures mid-STALL state and ERET resumes at wrong PC
;     (would break loop logic, mismatch or cycle limit)
;   - Cache fill races with IRQ entry (subtle data corruption)
;
; The patterns are 16 distinct words at addresses 0x100–0x13C.  The
; loop iterates over them ITERATIONS times.  Each load's value is
; compared to the address-derived expected value, so any wrong-data
; bug surfaces immediately.
;
; Result: R1=1 PASS, R1=0 FAIL (mismatch or timeout)

.equ DATA_BASE,    0x100
.equ ITERATIONS,   8

_start:
    LLI   R1, #0                  ; assume fail

    ; ── Install timer handler ──────────────────────────────
    LA    R2, #timer_handler
    STW   R2, [R0 + #4]

    ; ── Pre-populate 16 words with the pattern: addr | 0xA5000000 ──
    ; This gives each word a distinct, address-derived value so any
    ; mis-routed load is immediately detectable.
    LLI   R3, #DATA_BASE          ; current address
    LLI   R4, #16                 ; word count
    LI    R5, #0xA5000000         ; pattern OR'd into addr
fill_loop:
    OR    R5, R3                  ; pattern = 0xA5000000 | addr
    STW   R5, [R3]
    LI    R5, #0xA5000000         ; reset pattern for next OR
    ADD   R3, #4
    SUB   R4, #1
    BNE   fill_loop

    ; ── Configure timer ────────────────────────────────────
    LLI   R3, #3
    WRSYS R3, #TIMER, #TM_RELOAD
    WRSYS R3, #TIMER, #TM_COUNT
    LLI   R3, #0x07               ; TICK_EN | IRQ_EN | AUTOLOAD
    WRSYS R3, #TIMER, #TM_CR

    EI
    NOP

    ; ── Main test loop ─────────────────────────────────────
    LLI   R6, #ITERATIONS
outer_loop:
    LLI   R7, #DATA_BASE          ; current address
    LLI   R8, #16                 ; words this pass
inner_loop:
    LDW   R9, [R7]                ; load word (potential STALL point)
    ; expected = 0xA5000000 | R7
    LI    R10, #0xA5000000
    OR    R10, R7
    CMP   R9, R10
    BNE   fail                    ; corruption detected

    ADD   R7, #4
    SUB   R8, #1
    BNE   inner_loop

    SUB   R6, #1
    BNE   outer_loop

    ; ── All passes succeeded ───────────────────────────────
    LLI   R3, #0
    WRSYS R3, #TIMER, #TM_CR
    DI

    LLI   R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Timer handler — ack and return (no extra side effects, so a bug
; that corrupts loads must be coming from the STALL/IRQ interaction
; itself, not from the handler doing something dangerous)
; ═══════════════════════════════════════════════════════════════
timer_handler:
    LLI   R11, #1
    WRSYS R11, #TIMER, #TM_STATUS
    ERET
