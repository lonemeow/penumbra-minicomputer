; test_dcache.s — D-cache integration test
; REQUIRES: mmu cache
;
; Tests:
;   1. Read D-cache INFO sysreg, verify non-zero (geometry valid)
;   2. Identity-map page 0 as cacheable (C=1), enable MMU
;   3. Enable D-cache
;   4. Store a value, load it back (first read = cold miss → fill)
;   5. Load same address again (should hit in cache)
;   6. Store different value to same address (write-hit updates cache)
;   7. Load it back to verify write-hit updated cache correctly
;   8. Invalidate D-cache, load again (should miss, re-fill from RAM)
;   9. Verify data is still correct after re-fill
;
; The MMU must be enabled for cacheable accesses — in bypass mode,
; mmu_cacheable=0, so the cache passes everything through.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX_C, 0xBD     ; V|C|R|W|X|G — cacheable kernel page
.equ KERN_RWX,   0xB9     ; V|R|W|X|G   — uncacheable kernel page
.equ TEST_ADDR,  0x0800   ; test address within page 0
.equ TEST_VAL1,  0xCAFE
.equ TEST_VAL2,  0xBEEF

_start:
    LLI  R1, #0               ; assume fail

    ; ── 1. Read D-cache INFO sysreg ───────────────────────────
    RDSYS R2, #DCACHE, #CACHE_INFO
    CMP  R2, R0
    BEQ  fail                  ; INFO should be non-zero

    ; ── 2. Map page 0 as cacheable, ROM page uncacheable ──────
    ; Slot 0: VPN 0 → PPN 0, cacheable
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN       ; VPN=0, ASID=0
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE       ; PPN=0, cacheable

    ; Slot 16: ROM page (VPN 0xFFFF0 → PPN 0xFFFF0), uncacheable
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00B9           ; PPN=0xFFFF0, uncacheable (no C bit)
    WRSYS R2, #MMU, #TLB_PTE

    ; Enable MMU
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── 3. Enable D-cache ─────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL

    ; ── 4. Store and load (cold miss → fill → hit on re-eval) ─
    LLI  R5, #TEST_VAL1
    LLI  R6, #TEST_ADDR
    STW  R5, [R6]                  ; write-miss (write-no-allocate: goes to RAM)
    LDW  R7, [R6]                  ; read-miss → line fill from RAM → re-hit
    CMP  R7, R5
    BNE  fail

    ; ── 5. Read same address again (cache hit) ────────────────
    LDW  R8, [R6]
    CMP  R8, R5
    BNE  fail

    ; ── 6. Store new value (write-hit: updates cache + RAM) ───
    LLI  R5, #TEST_VAL2
    STW  R5, [R6]
    LDW  R7, [R6]                  ; should read updated value from cache
    CMP  R7, R5
    BNE  fail

    ; ── 7. Invalidate D-cache ─────────────────────────────────
    WRSYS R0, #DCACHE, #CACHE_INVAL_ALL

    ; ── 8. Load after invalidate (miss → re-fill from RAM) ────
    LDW  R7, [R6]
    CMP  R7, R5                    ; RAM should have TEST_VAL2 (write-through)
    BNE  fail

    ; ── 9. Disable D-cache, disable MMU, verify RAM directly ──
    LLI  R4, #0
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #MMU, #MMUCR         ; MMU off → bypass mode

    LDW  R7, [R6]                  ; direct RAM read (bypass, uncached)
    CMP  R7, R5
    BNE  fail

    ; ── All checks passed ─────────────────────────────────────
    LLI  R1, #1                    ; PASS
fail:
    BREAK
