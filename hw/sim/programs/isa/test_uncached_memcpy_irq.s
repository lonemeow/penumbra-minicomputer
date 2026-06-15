; test_uncached_memcpy_irq.s — memcpy through UNCACHEABLE pages with timer IRQs
; REQUIRES: mmu timer irq
;
; The device-access companion to test_cache_memcpy.s / test_cache_memcpy_irq.s.
; Same access pattern (LDW/STW through MMU-translated VAs with timer IRQs firing
; during the loop), but the SRC/DST data pages are mapped C=0, so every access
; is a non-cacheable single-beat bus round-trip — tens of cycles, no cache hit.
;
; That makes the loop spend most of its time *inside* a stalled data access, so
; a timer IRQ almost always lands while one is in flight. A pipelined core must
; not take the interrupt then: a non-idempotent access that has issued cannot be
; squashed (re-executing it would double the side effect), and one that has not
; issued must not be torn out of the loop. The interrupt must wait for the
; in-flight access to retire, then resume the loop at the next instruction. If it
; cuts mid-access the copy desyncs and the verify below catches it.
;
; The handler does the minimum to ack the timer and return; it touches no data
; pages of its own, so any corruption comes from the interrupt's interaction with
; the in-flight uncacheable access, not from handler-side memory.
;
; Result: R1 = 1 PASS, R1 = 0 FAIL (data mismatch, or no IRQs fired — which would
; mean the test did not exercise the intended interleaving).

.equ N_WORDS,    256          ; 1 KiB — kept modest: each access is a bus round-trip
.equ SRC_PA,     0x1000
.equ DST_PA,     0x2000
.equ SRC_VA,     0x1000
.equ DST_VA,     0x2000
.equ KERN_RWX_C, 0xBD         ; cacheable flags: page 0 (vectors) only
                              ; SRC/DST use 0x..B9 — the same flags with C=0 (uncacheable)

; Timer reload — tuned so the timer fires many times across the (slow,
; uncacheable) copy, landing inside an in-flight access with high probability.
.equ TIMER_RELOAD, 50

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

    ; ── Phase 2: set up TLB — page 0 cacheable, SRC/DST uncacheable ──
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100                  ; VPN 1 (SRC page)
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10B9                  ; PPN 1 << 12 | uncacheable (C=0) flags
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200                  ; VPN 2 (DST page)
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x20B9                  ; PPN 2 << 12 | uncacheable (C=0) flags
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

    ; ── Enable MMU (D-cache stays off — data is uncacheable) ─
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; ── Enable interrupts ────────────────────────────────────
    EI
    NOP                        ; ei_shadow consumed

    ; ── Phase 3: memcpy src → dst through uncacheable MMU, with
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
    ; Otherwise the test did not exercise the intended interleaving.
    CMP  R11, R0
    BEQ  fail

    ; All checks passed
    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; IRQ handler — minimum work to ack timer and return. Clobbers R10
; (status clear) and R11 (IRQ count); both are unused by the memcpy
; loop, so the loop's R5/R6/R7/R8 state survives the handler intact.
; ═══════════════════════════════════════════════════════════════
irq_handler:
    LLI   R10, #1
    WRSYS R10, #TIMER, #TM_STATUS   ; write-1-to-clear underflow
    ADD   R11, #1                    ; bump IRQ counter
    ERET
