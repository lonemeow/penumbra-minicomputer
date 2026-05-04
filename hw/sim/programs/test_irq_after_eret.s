; test_irq_after_eret.s — IRQ pending during kernel-mode trap, taken on
; the first dispatch after ERET back to the trap-originator
;
; Sequence under test:
;   1. _start configures the timer to fire shortly (reload=5 → ~125 CPU
;      cycles), enables interrupts, and immediately issues SYSCALL.
;   2. The SYSCALL handler runs in supervisor mode with I=0; the timer
;      fires inside the handler but is held off by I=0.
;   3. The handler spins long enough that timer is definitely pending,
;      advances EPC past the SYSCALL, and ERETs.
;   4. ERET restores SR (S=1, I=1), so the very next dispatch sees a
;      pending timer IRQ — vector_num must select VEC_TIMER and
;      transfer to the timer handler before the post-SYSCALL NOPs run.
;   5. The timer handler clears status, disables the timer, sets R1=1
;      and BREAKs — proof that the chain executed correctly.
;
; Failure modes this test catches:
;   - ESR.I not restored on ERET (would leave I=0, IRQ never fires,
;     post-SYSCALL BREAK reaches with R1=0)
;   - ei_shadow erroneously set after ERET (would delay IRQ by one
;     instruction; with multiple post-ERET NOPs the timer would still
;     fire before BREAK, so this case still passes — see the companion
;     test if tighter validation is needed)
;   - Timer IRQ lost while held off in supervisor mode (would leave
;     timer_irq=0 at ERET — but the timer device asserts level-style,
;     not edge, so this requires a bug in the timer device itself)
;
; Result: R1=1 PASS (timer handler reached), R1=0 FAIL (post-SYSCALL
;          BREAK reached because IRQ never fired)

_start:
    LLI   R1, #0                  ; assume fail

    ; ── Install vector table ────────────────────────────────
    LA    R2, #timer_handler
    STW   R2, [R0 + #4]           ; vector[1]  = VEC_TIMER

    LA    R2, #syscall_handler
    STW   R2, [R0 + #0x14]        ; vector[5]  = VEC_SYSCALL

    ; ── Configure timer: reload=5, IRQ enabled, auto-load ────
    ; Tick freq ~1 MHz at 25 MHz CPU → reload=5 → ~5 µs = ~125
    ; CPU cycles per timer underflow.  Plenty of headroom for
    ; the SYSCALL handler's spin to complete.
    LLI   R3, #5
    WRSYS R3, #TIMER, #TM_RELOAD
    WRSYS R3, #TIMER, #TM_COUNT
    LLI   R3, #0x07               ; TICK_EN | IRQ_EN | AUTOLOAD
    WRSYS R3, #TIMER, #TM_CR

    ; ── Enable interrupts ───────────────────────────────────
    EI
    NOP                            ; ei_shadow consumed here

    ; ── Trap into kernel; timer must fire while we're inside ─
    SYSCALL

    ; ── Post-SYSCALL landing zone ───────────────────────────
    ; If ERET semantics + IRQ dispatch are correct, the timer
    ; handler runs before any NOP here even completes its own
    ; commit.  The NOPs serve as a buffer in case ei_shadow
    ; delays IRQ by one instruction (still pass — handler
    ; runs before BREAK).  The BREAK at the end is the
    ; failure trap.
    NOP
    NOP
    NOP
    NOP
    BREAK                          ; FAIL: IRQ never fired

; ═══════════════════════════════════════════════════════════════
; SYSCALL handler — runs in S=1, I=0
; ═══════════════════════════════════════════════════════════════
syscall_handler:
    ; Advance EPC past the SYSCALL so ERET lands on the NOPs.
    RDSPR R8, EPC
    ADD   R8, #4
    WRSPR EPC, R8

    ; Spin for ~300 CPU cycles — well past the timer's ~125-cycle
    ; period — so timer_irq is definitely pending by the time we
    ; ERET.  100 iterations of (SUB, BNE) ≈ 300 cycles.
    LLI   R9, #100
sysc_delay:
    SUB   R9, #1
    BNE   sysc_delay

    ERET

; ═══════════════════════════════════════════════════════════════
; Timer handler — runs in S=1, I=0
; ═══════════════════════════════════════════════════════════════
timer_handler:
    ; Clear the underflow flag (write-1-to-clear).
    LLI   R10, #1
    WRSYS R10, #TIMER, #TM_STATUS

    ; Disable the timer so we don't keep firing.
    LLI   R10, #0
    WRSYS R10, #TIMER, #TM_CR

    ; PASS — we got here via timer IRQ taken first dispatch after ERET.
    LLI   R1, #1
    BREAK
