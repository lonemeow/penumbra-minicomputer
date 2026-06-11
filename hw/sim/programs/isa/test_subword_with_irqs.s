; test_subword_with_irqs.s — halfword loop with timer IRQs firing during it
; REQUIRES: mmu cache l2 timer irq
;
; Userland code under -O2 runs with hardware timer IRQs enabled and
; firing regularly.  A trap entry mid-loop saves+restores the CPU
; state, including any in-flight sub-word load result.  No existing
; test combines:
;   - sub-word load loop
;   - timer IRQ firing during the loop
;   - both L1 caches + L2 enabled
;
; This test pins those together: tight halfword walk over a 1 KiB
; region with timer firing every ~75 cycles, verifying both that the
; loop reads correct values AND that the IRQ handler fired.  If the
; trapframe save/restore corrupts a register that the loop depends
; on (e.g. the halfword pointer or running expected value), the
; verify CMP will fail.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C,   0xBD
.equ DATA_BASE,    0x0100
.equ DATA_END,     0x0500          ; 1 KiB = 512 halfwords
.equ COUNTER_ADDR, 0x0040          ; in page 0, away from data region
                                   ; (vector table 0x00..0x28; counter at 0x40)

_start:
    LLI  R1, #0

    ; ── Install timer handler (vector 1 → physical 0x04) ─────────
    LA   R2, #timer_handler
    STW  R2, [R0 + #4]

    ; ── Clear counter ────────────────────────────────────────────
    STW  R0, [R0 + #COUNTER_ADDR]

    ; ── MMU setup ────────────────────────────────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00B9
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Enable all caches ────────────────────────────────────────
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2,     #CACHE_CTRL

    ; ── Initialize: A[i] = i + 1  (halfword index 0..511) ────────
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #1
init_loop:
    STH  R7, [R5]
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  init_loop

    ; ── Configure + enable timer ─────────────────────────────────
    LLI  R3, #2
    WRSYS R3, #TIMER, #TM_RELOAD
    WRSYS R3, #TIMER, #TM_COUNT
    LLI  R3, #0x07              ; TICK_EN | IRQ_EN | AUTOLOAD
    WRSYS R3, #TIMER, #TM_CR

    EI

    ; ── Forward halfword walk under active IRQ load ──────────────
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #1
walk:
    LDH  R2, [R5]
    CMP  R2, R7
    BNE  fail
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  walk

    ; ── Disable timer ────────────────────────────────────────────
    LLI  R3, #0
    WRSYS R3, #TIMER, #TM_CR
    DI

    ; ── Verify the handler actually fired (counter > 0) ──────────
    LDW  R9, [R0 + #COUNTER_ADDR]
    CMP  R9, R0
    BEQ  fail                   ; counter == 0 → IRQ never fired → test invalid

    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Timer handler — increment counter, clear UDF, return.
; Must preserve every register the main loop depends on.
; ═══════════════════════════════════════════════════════════════
timer_handler:
    LDW  R9, [R0 + #COUNTER_ADDR]
    ADD  R9, #1
    STW  R9, [R0 + #COUNTER_ADDR]

    LLI  R10, #1
    WRSYS R10, #TIMER, #TM_STATUS

    ERET
