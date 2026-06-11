; test_subword_sdram_cached.s — sub-word loads/stores through full cache stack
; REQUIRES: mmu cache l2
;
; Existing test_subword_load.s / test_subword_store.s run with MMU off
; and caches at reset state (disabled), so they exercise only the raw
; byte_ext/byte_rep paths against the bus.  This test enables the
; identity-mapped MMU, both L1 caches, and the L2 cache, then runs the
; same sub-word load/store matrix against SDRAM — covering the full
; CPU → L1 → L2 → adapter → CDC → SDRAM path that real userland uses.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C, 0xBD              ; V|C|R|W|X|G — cacheable kernel page
.equ DATA_BASE,  0x0100            ; well past vector table

_start:
    LLI  R1, #0                    ; assume fail until proven otherwise

    ; ── MMU: identity-map page 0 cacheable, ROM page uncacheable ─
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

    ; ── Enable all three caches (D, I, L2) ───────────────────────
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2,     #CACHE_CTRL

    ; ── Store known word pattern at DATA_BASE ────────────────────
    LLI  R8, #0x0281
    LUI  R8, #0x7FFF               ; R8 = 0x7FFF0281
    LLI  R9, #DATA_BASE
    STW  R8, [R9]

    ; ── Byte loads (LDB, zero-extend) at every offset ────────────
    LDB  R2, [R9]
    CMP  R2, #0x81
    BNE  fail
    LDB  R2, [R9 + #1]
    CMP  R2, #2
    BNE  fail
    LDB  R2, [R9 + #2]
    CMP  R2, #0xFF
    BNE  fail
    LDB  R2, [R9 + #3]
    CMP  R2, #0x7F
    BNE  fail

    ; ── Byte loads (LDBS, sign-extend) ───────────────────────────
    LDBS R2, [R9]                  ; 0x81 → 0xFFFFFF81
    LLI  R3, #0xFF81
    LUI  R3, #0xFFFF
    CMP  R2, R3
    BNE  fail
    LDBS R2, [R9 + #2]             ; 0xFF → 0xFFFFFFFF
    LI   R3, #0xFFFFFFFF
    CMP  R2, R3
    BNE  fail
    LDBS R2, [R9 + #3]             ; 0x7F → 0x0000007F
    CMP  R2, #0x7F
    BNE  fail

    ; ── Halfword loads (LDH, zero-extend) ────────────────────────
    LDH  R2, [R9]
    CMP  R2, #0x0281
    BNE  fail
    LDH  R2, [R9 + #2]
    CMP  R2, #0x7FFF
    BNE  fail

    ; ── Halfword loads (LDHS, sign-extend) ───────────────────────
    LLI  R8, #0x8000
    LUI  R8, #0xFFFE               ; R8 = 0xFFFE8000
    STW  R8, [R9]

    LDHS R2, [R9]                  ; 0x8000 → 0xFFFF8000
    LLI  R3, #0x8000
    LUI  R3, #0xFFFF
    CMP  R2, R3
    BNE  fail

    LDHS R2, [R9 + #2]             ; 0xFFFE → 0xFFFFFFFE
    LLI  R3, #0xFFFE
    LUI  R3, #0xFFFF
    CMP  R2, R3
    BNE  fail

    ; ── STB at every byte offset, verify isolation ───────────────
    LI   R8, #0xFFFFFFFF
    STW  R8, [R9]

    LLI  R2, #0x42
    STB  R2, [R9]
    LDW  R3, [R9]
    LLI  R4, #0xFF42
    LUI  R4, #0xFFFF
    CMP  R3, R4
    BNE  fail

    STW  R8, [R9]                  ; refill
    LLI  R2, #0xAB
    STB  R2, [R9 + #2]
    LDW  R3, [R9]
    LLI  R4, #0xFFFF
    LUI  R4, #0xFFAB
    CMP  R3, R4
    BNE  fail

    ; ── STH at both halfword offsets ─────────────────────────────
    LLI  R8, #0
    STW  R8, [R9]
    LLI  R2, #0xBEEF
    STH  R2, [R9]
    LDW  R3, [R9]
    CMP  R3, #0xBEEF
    BNE  fail

    LI   R8, #0xFFFFFFFF
    STW  R8, [R9]
    LLI  R2, #0x1234
    STH  R2, [R9 + #2]
    LDW  R3, [R9]
    LLI  R4, #0xFFFF
    LUI  R4, #0x1234
    CMP  R3, R4
    BNE  fail

    ; All passed
    LLI  R1, #1
fail:
    BREAK
