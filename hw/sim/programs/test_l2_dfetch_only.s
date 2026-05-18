; test_l2_dfetch_only.s — same body as test_l2_ifetch, but with the
; ROM page UNCACHEABLE so the I-fetch stream bypasses L1-I + L2 and
; only D-cache traffic goes through the L2.  Use this to bisect
; whether the L2 integration bug needs I-fetch interleave to fire
; or whether it's pure D-side.

.equ KERN_RWX_C, 0xBD     ; V|C|R|W|X|G — cacheable kernel page
.equ N_ITERS,    32
.equ DATA_BASE,  0x0400

_start:
    LLI  R1, #0

    ; Plant data 1..32
    LLI  R5, #DATA_BASE
    LLI  R6, #0
plant:
    ADD  R6, #1
    STW  R6, [R5]
    ADD  R5, #4
    CMP  R6, #N_ITERS
    BNE  plant

    ; TLB: VPN 0 cacheable
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    ; ROM page UNCACHEABLE  (only difference from test_l2_ifetch.s)
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00B9       ; no C bit
    WRSYS R2, #MMU, #TLB_PTE

    ; Bring-up: D-cache, I-cache, MMU, L2
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #MMU, #MMUCR

    RDSYS R2, #L2, #CACHE_INFO
    CMP  R2, R0
    BEQ  no_l2
    WRSYS R4, #L2, #CACHE_CTRL
no_l2:

    LLI  R5, #DATA_BASE
    LLI  R6, #0
    LLI  R7, #N_ITERS
loop:
    LDW  R8, [R5]
    ADD  R6, R8
    ADD  R5, #4
    SUB  R7, #1
    BNE  loop

    LLI  R9, #0x210
    CMP  R6, R9
    BNE  fail

    LLI  R1, #1
fail:
    BREAK
