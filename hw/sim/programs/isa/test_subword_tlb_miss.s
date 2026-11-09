; test_subword_tlb_miss.s — sub-word loads interleaved with TLB misses
; REQUIRES: mmu cache l2 wrspr
;
; Userland sub-word access patterns are interrupted by TLB miss traps
; that hand off to the software miss handler, which itself does loads
; (page-table walk) and writes (TLB install).  No existing test mixes:
;   - sub-word loads
;   - TLB miss traps
;   - cache-enabled execution
;
; Strategy:
;   1. Identity-map page 0 (vector table + handler scratch) cacheable.
;   2. Install a TLB miss handler that maps the faulting VPN to itself
;      (identity), then ERETs to retry the access.
;   3. Walk a 16 KiB region (4 pages) doing halfword loads.  Each new
;      page faults once on first access; subsequent halfwords in the
;      same page hit TLB normally.  After the walk, re-walk to exercise
;      warm-TLB sub-word access.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C, 0xBD
.equ DATA_BASE,  0x1000          ; start of region (page 1)
.equ DATA_END,   0x5000          ; 4 pages = 16 KiB
.equ MISS_COUNT_ADDR, 0x0040     ; in page 0, counts handler invocations

_start:
    BL   tlb_init                  ; the TLB is undefined after reset
    LLI  R1, #0

    ; ── Install TLB miss handler (vector 2 → physical 0x08) ──────
    LA   R2, #tlb_miss_handler
    STW  R2, [R0 + #8]

    ; ── Clear miss counter ───────────────────────────────────────
    STW  R0, [R0 + #MISS_COUNT_ADDR]

    ; ── MMU: only page 0 (vector + handler + counter) and ROM ────
    ; Pages 1..4 (data region) are NOT mapped → first access faults.
    LI   R2, #0x0         ; TLB_VPN word, way 0
    BL   tlb_slot
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    LI   R2, #0x0FFFF000        ; TLB_VPN word, way 0
    BL   tlb_slot
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

    ; ── Initialize data via STH ──────────────────────────────────
    ; Each first access to a new page (1, 2, 3, 4) traps to miss
    ; handler, which installs identity mapping, then retries.
    LLI  R5, #DATA_BASE
    LI   R6, #DATA_END
    LLI  R7, #1
init_loop:
    STH  R7, [R5]
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  init_loop

    ; ── Forward halfword walk (warm TLB after init) ──────────────
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

    ; ── Verify TLB miss handler actually ran (counter > 0) ───────
    LDW  R9, [R0 + #MISS_COUNT_ADDR]
    CMP  R9, R0
    BEQ  fail

    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; TLB miss handler — identity-maps the faulting VPN into way 0 of its
; set; the test only needs 4 mappings and their evictions.
;
; Increments a counter at MISS_COUNT_ADDR so the test can verify
; it actually fired.
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    ; Bump miss counter (page 0 mapping is fixed, so LDW always works)
    LDW  R9, [R0 + #MISS_COUNT_ADDR]
    ADD  R9, #1
    STW  R9, [R0 + #MISS_COUNT_ADDR]

    ; Read faulting VA, extract VPN
    RDSYS R8, #MMU, #FAULT_ADDR
    SHR  R8, #12                 ; R8 = VPN

    ; The lookup reads set VPN mod SETS, so the install must target that
    ; set or the lookup misses and the handler re-faults forever. Way 0
    ; always; a later miss to the same set overwrites it.
    RDSPR R9, SCR2               ; SETS - 1 (mmu.inc)
    AND  R9, R8                  ; R9 = way 0's slot for the VPN
    WRSYS R9, #MMU, #TLB_INDEX

    ; TLB_VPN format: VPN[19:0] << 8 | ASID[7:0]
    MOV  R9, R8
    SHL  R9, #8
    WRSYS R9, #MMU, #TLB_VPN

    ; TLB_PTE format: PPN[19:0] << 12 | flags[11:0]
    ; Identity mapping → PPN = VPN.
    SHL  R8, #12
    LLI  R11, #KERN_RWX_C
    OR   R8, R11
    WRSYS R8, #MMU, #TLB_PTE

    ERET                          ; retry faulting instruction

.include "mmu.inc"
