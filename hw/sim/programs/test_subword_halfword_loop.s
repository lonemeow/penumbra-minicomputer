; test_subword_halfword_loop.s — tight halfword-pointer-increment loop
;
; Mirrors the hot inner loop of NetBSD libc's __hash_get / hash_access:
;   bp = (uint16_t *)page; for (...) { ... bp[1] ... bp += 2; }
; This is the access pattern that triggered the SIGABRT in /sbin/init
; at -O2 — kernel code virtually never does halfword traversal, so
; this path has effectively zero coverage in the existing test suite.
;
; Strategy:
;   1. Identity-map page 0 cacheable, enable both L1 caches + L2.
;   2. Initialize a 1 KiB region with halfword pattern A[i] = i + 1
;      (so each halfword's expected value is also its index).
;   3. Tight loop: ldh r2, [r5]; cmp r2, r6; bne fail; add r5, #2; ...
;   4. Then re-walk it backwards (bp -= 2) to exercise the reverse
;      access pattern too.
;   5. Re-invalidate caches and re-walk forwards to catch cold-fill
;      bugs separate from warm-cache bugs.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C, 0xBD
.equ DATA_BASE,  0x0100
.equ DATA_END,   0x0500            ; 1 KiB region = 512 halfwords

_start:
    LLI  R1, #0

    ; ── MMU: identity-map page 0 cacheable, ROM uncacheable ──────
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

    ; ── Enable all three caches ──────────────────────────────────
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2,     #CACHE_CTRL

    ; ── Initialize: A[i] = i + 1  (i = halfword index, 0..511) ───
    ; Store as halfwords so we exercise STH on every offset.  Pattern
    ; chosen so any swap/replace bug is diagnosable from the value:
    ;   - off-by-one shows up as A[i] = i or i + 2
    ;   - half-swap shows up as bytes reversed
    ;   - adjacent-line corruption shows up as A[i] = A[i ± k]
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #1                    ; running halfword value
init_loop:
    STH  R7, [R5]
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  init_loop

    ; ── Pass 1: forward halfword walk (warm cache for upper half) ─
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #1
fwd_loop:
    LDH  R2, [R5]
    CMP  R2, R7
    BNE  fail
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  fwd_loop

    ; ── Pass 2: backward halfword walk (different prefetch pattern)
    LLI  R5, #DATA_END
    SUB  R5, #2                    ; last halfword
    LLI  R6, #DATA_BASE
    LLI  R7, #512                  ; last expected value (i = 511, value = 512)
bwd_loop:
    LDH  R2, [R5]
    CMP  R2, R7
    BNE  fail
    SUB  R5, #2
    SUB  R7, #1
    CMP  R5, R6
    BHS  bwd_loop                  ; stops when R5 < DATA_BASE
    ; Final check: R5 == DATA_BASE - 2 (already verified A[0]), no extra ldh

    ; ── Pass 3: cold-fill walk (invalidate caches, walk forward) ──
    WRSYS R0, #DCACHE, #CACHE_INVAL_ALL
    WRSYS R0, #ICACHE, #CACHE_INVAL_ALL
    WRSYS R0, #L2,     #CACHE_INVAL_ALL

    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #1
cold_loop:
    LDH  R2, [R5]
    CMP  R2, R7
    BNE  fail
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  cold_loop

    LLI  R1, #1
fail:
    BREAK
