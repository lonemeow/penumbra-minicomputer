; cache_ifetch.s -- instruction fetches whose cache outcomes are known per
; generation.
;
; The loop below runs from a cacheable code page and spans two lines 1 KiB
; apart: `line_a` at 0xFFFF1000 and `line_b` at 0xFFFF1400.  Everything
; else runs from the uncached ROM page, so only the loop's 33 fetches --
; ten rounds of SUB, B, BNE, then the exit's LLI, LUI, JMP -- reach the
; modelled caches.  In a 1 KiB direct-mapped L1-I the two lines share a
; set and evict each other every round; in a 4 KiB 2-way one they sit in
; different sets.  run_iss_tests.py derives the counts.

.equ ROM_PTE,   0xFFFF00A9     ; V|R|X|G, uncached
.equ LOOP_PTE,  0xFFFF10AD     ; V|C|R|X|G

_start:
    ; Pinned slot 0: the ROM page, uncached
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE
    ; Pinned slot 1: the loop's page, cacheable
    LLI  R2, #0x41
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF100
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #LOOP_PTE
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    LLI  R4, #10
    LA   R2, #line_a
    JMP  R2
back:
    BREAK

.org 0xFFFF1000
line_a:
    SUB  R4, #1
    B    line_b

.org 0xFFFF1400
line_b:
    BNE  line_a
    LA   R2, #back
    JMP  R2
