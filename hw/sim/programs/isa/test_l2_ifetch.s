; test_l2_ifetch.s — exercise I-fetch through L2
; REQUIRES: mmu cache l2
;
; The minimal L2 test (test_l2_minimal.s) only enables D-cache and
; routes data through L2.  It passes with HAS_L2=1, but the
; benchmarks still hang.  Key difference: the benchmark crt0 also
; enables the I-cache before turning on L2, so I-fetch line fills
; flow CPU → L1 I-cache → arbiter → L2 → memory.  This test
; reproduces that path.
;
; Strategy:
;   1. Identity-map page 0 cacheable, plus a cacheable ROM page
;      (so I-fetch from 0xFFFF_xxxx counts as cacheable for L2).
;   2. Bring up: D-cache, I-cache, MMU, then L2  (matches crt0 order).
;   3. Run a small loop after L2 is on — each iteration re-fetches
;      the loop body, exercising I-cache hits (and L2 hits if the
;      I-cache evicts).  Also do a few cached loads inside the loop.
;   4. Check the loop counter and accumulator at the end.
;
; If I-fetch through L2 returns wrong instructions, the loop either
; hangs (jumps to garbage) or produces wrong sum.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX_C, 0xBD     ; V|C|R|W|X|G — cacheable kernel page
.equ N_ITERS,    32       ; loop iterations
.equ DATA_BASE,  0x0400   ; where we plant the inputs

_start:
    LLI  R1, #0                ; assume fail

    ; ── Plant data: 32 words = i+1 at DATA_BASE (MMU off) ─────
    LLI  R5, #DATA_BASE
    LLI  R6, #0
plant:
    ADD  R6, #1
    STW  R6, [R5]
    ADD  R5, #4
    CMP  R6, #N_ITERS
    BNE  plant

    ; ── TLB: VPN 0 cacheable, ROM page cacheable ──────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00BD       ; ROM page CACHEABLE (was 0xB9)
    WRSYS R2, #MMU, #TLB_PTE

    ; ── Bring-up order: caches, MMU, L2 (mirrors crt0) ────────
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #MMU, #MMUCR

    RDSYS R2, #L2, #CACHE_INFO
    CMP  R2, R0
    BEQ  no_l2
    WRSYS R4, #L2, #CACHE_CTRL
no_l2:

    ; ── Sum loop (I-fetch goes through L1 I-cache then L2) ────
    LLI  R5, #DATA_BASE        ; data pointer
    LLI  R6, #0                ; accumulator
    LLI  R7, #N_ITERS          ; remaining
loop:
    LDW  R8, [R5]
    ADD  R6, R8
    ADD  R5, #4
    SUB  R7, #1
    BNE  loop

    ; Expected sum: 1+2+...+32 = 32*33/2 = 528 = 0x210
    LLI  R9, #0x210
    CMP  R6, R9
    BNE  fail

    LLI  R1, #1
fail:
    BREAK
