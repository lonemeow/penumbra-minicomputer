; test_subword_multipage.s — sub-word loads across multiple pre-mapped pages
; REQUIRES: mmu cache l2
;
; Same shape as test_subword_halfword_loop but spans 4 pages (16 KiB)
; instead of staying in page 0.  No TLB miss handler — all 4 data pages
; are pre-installed in main TLB slots so accesses always hit.  Isolates
; "sub-word op + multi-page traversal" from any TLB-miss-handler effects.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C, 0xBD
.equ DATA_BASE,  0x1000          ; page 1
.equ DATA_END,   0x5000          ; through page 4

_start:
    LLI  R1, #0

    ; ── MMU: identity-map pages 0,1,2,3,4 cacheable + ROM ───────
    ; Slot computed from VPN[4:0]:  page N → slot N (way 0)
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0
    WRSYS R3, #MMU, #TLB_VPN          ; VPN 0 << 8 = 0
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE          ; PPN 0 << 12 | flags

    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100                  ; VPN 1 << 8 = 0x100
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0x000010BD              ; PPN 1 << 12 | flags
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0x000020BD
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #3
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0300
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0x000030BD
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #4
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0400
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0x000040BD
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

    ; ── Initialize: A[i] = i + 1 across all 4 data pages ─────────
    LLI  R5, #DATA_BASE
    LI   R6, #DATA_END
    LLI  R7, #1
init:
    STH  R7, [R5]
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  init

    ; ── Forward walk ─────────────────────────────────────────────
    LLI  R5, #DATA_BASE
    LI   R6, #DATA_END
    LLI  R7, #1
walk:
    LDH  R2, [R5]
    CMP  R2, R7
    BNE  fail
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  walk

    LLI  R1, #1
fail:
    BREAK
