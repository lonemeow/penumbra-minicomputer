; test_cache_memcpy_irq.s — sustained cached memcpy with timer IRQs
;                          firing during the loop.
;
; Same access pattern as test_cache_memcpy.s (LDW/STW through cached
; MMU-translated VAs), but with timer IRQs enabled at high frequency
; during the memcpy.  Tests whether IRQ delivery interleaved with
; cache fill bursts is the trigger for the residual NetBSD-userspace
; bug that the supervisor-only-memcpy test does not catch.
;
; The handler does the minimum to acknowledge the IRQ and return:
; clears the underflow flag, increments an IRQ counter (R11), ERETs.
; It deliberately touches no D-cache state of its own — any
; corruption to the memcpy loop's data must come from the cache /
; arbiter / adapter interaction with the privilege transition
; itself, not from handler-side memory accesses.
;
; Result: R1 = 1 PASS, R1 = 0 FAIL (either data mismatch or no
; IRQs fired during the memcpy, which would mean the test didn't
; exercise the intended path).

.equ N_WORDS,    1024         ; full 4 KiB page
.equ SRC_PA,     0x1000
.equ DST_PA,     0x2000
.equ SRC_VA,     0x1000
.equ DST_VA,     0x2000
.equ KERN_RWX_C, 0xBD

; Timer reload — tuned so the timer fires several times during the
; ~30k-cycle memcpy.  reload=100 → ~2500 CPU cycles between fires,
; ~12 fires across the memcpy.
.equ TIMER_RELOAD, 100

_start:
    LLI  R1, #0                ; assume FAIL until everything passes
    LLI  R11, #0               ; IRQ counter

    ; ── Install timer IRQ handler at vector 1 ────────────────
    LA   R2, #irq_handler
    STW  R2, [R0 + #4]         ; vector[1] = IRQ

    ; ── Phase 1: plant pattern at SRC_PA (MMU off) ───────────
    LLI  R5, #SRC_PA
    LLI  R6, #0
plant_loop:
    LUI  R7, #0x8000
    OR   R7, R6
    STW  R7, [R5]
    ADD  R5, #4
    ADD  R6, #1
    CMP  R6, #N_WORDS
    BNE  plant_loop

    ; ── Phase 2: set up TLB ──────────────────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10BD
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x20BD
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Configure timer (auto-load, IRQ enabled) ─────────────
    LLI  R3, #TIMER_RELOAD
    WRSYS R3, #TIMER, #TM_RELOAD
    WRSYS R3, #TIMER, #TM_COUNT
    LLI  R3, #0x07             ; TICK_EN | IRQ_EN | AUTOLOAD
    WRSYS R3, #TIMER, #TM_CR

    ; ── Enable MMU + D-cache ─────────────────────────────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR
    WRSYS R6, #DCACHE, #CACHE_CTRL

    ; ── Enable interrupts ────────────────────────────────────
    EI
    NOP                        ; ei_shadow consumed

    ; ── Phase 3: memcpy src → dst through cached MMU, with
    ;   timer IRQs firing in the background. ──────────────────
    LLI  R5, #SRC_VA
    LLI  R6, #DST_VA
    LLI  R8, #0
memcpy_loop:
    LDW  R7, [R5]
    STW  R7, [R6]
    ADD  R5, #4
    ADD  R6, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  memcpy_loop

    ; ── Stop further IRQs ────────────────────────────────────
    DI
    LLI  R3, #0
    WRSYS R3, #TIMER, #TM_CR

    ; ── Phase 4: verify dst against expected pattern ─────────
    LLI  R5, #DST_VA
    LLI  R8, #0
verify_loop:
    LDW  R7, [R5]
    LUI  R9, #0x8000
    OR   R9, R8
    CMP  R7, R9
    BNE  fail
    ADD  R5, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  verify_loop

    ; ── Sanity check: at least one IRQ must have fired ───────
    ; If not, the test didn't actually exercise the intended
    ; path (timer interleaved with memcpy).  Treat as FAIL so a
    ; future timer-config regression doesn't silently turn this
    ; test into "memcpy without IRQ."
    CMP  R11, R0
    BEQ  fail

    ; All checks passed
    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; IRQ handler — minimum work to ack timer and return.
;
; Clobbers R10 (status clear) and R11 (IRQ count).  Both are
; unused by the memcpy loop, so the loop's R5/R6/R7/R8/R9 state
; survives the handler intact.
; ═══════════════════════════════════════════════════════════════
irq_handler:
    LLI   R10, #1
    WRSYS R10, #TIMER, #TM_STATUS   ; write-1-to-clear underflow
    ADD   R11, #1                    ; bump IRQ counter
    ERET
