; test_l2_delay — same as test_l2_n17 but with a long busy-wait after
; cache enable, so the walker is *long* done before we touch data.
;
; If the bug is "first cached miss after walker completes" (regardless
; of how long ago the walker finished), this fails identically to N=17.
; If the bug is "race at the walker-completion edge with concurrent
; bus activity", this passes.

.equ KERN_RWX_C, 0xBD
.equ N_ITERS,    17
.equ DATA_BASE,  0x0400

_start:
    LLI  R1, #0

    LLI  R5, #DATA_BASE
    LLI  R6, #0
plant:
    ADD  R6, #1
    STW  R6, [R5]
    ADD  R5, #4
    CMP  R6, #N_ITERS
    BNE  plant

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
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #MMU, #MMUCR

    RDSYS R2, #L2, #CACHE_INFO
    CMP  R2, R0
    BEQ  no_l2
    WRSYS R4, #L2, #CACHE_CTRL
no_l2:

    ; ── Long wait so walker is well past — ~3000 cycles ───────
    LLI  R10, #0
    LI   R11, #2000
wait_loop:
    ADD  R10, #1
    CMP  R10, R11
    BNE  wait_loop

    ; ── Now do exactly one new-line read (line 0x440, val 17) ─
    LLI  R6, #0
    LLI  R5, #0x440
    LDW  R8, [R5]
    ADD  R6, R8

    LLI  R9, #17
    CMP  R6, R9
    BNE  fail

    LLI  R1, #1
fail:
    BREAK
