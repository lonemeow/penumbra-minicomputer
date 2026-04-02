; test_icache.s — I-cache integration test
;
; Tests:
;   1. Read I-cache INFO sysreg, verify non-zero
;   2. Copy a straight-line function from ROM to cacheable RAM
;   3. Identity-map page 0 as cacheable, enable MMU + I-cache
;   4. Call RAM function — first call fills I-cache lines
;   5. Call same function again — fetches hit in I-cache
;   6. Invalidate I-cache, call again — re-fills from RAM
;   7. Verify all three calls produced correct results
;
; The RAM function does: R5 = (R5 + 3) * 2 - 1, using enough
; instructions to span at least one cache line (4 words = 16 bytes).
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX_C, 0xBD     ; V|C|R|W|X|G — cacheable kernel page
.equ KERN_RWX,   0xB9     ; V|R|W|X|G   — uncacheable kernel page
.equ RAM_DEST,   0x0800   ; within VPN 0 (0x0000–0x0FFF)

_start:
    LLI  R1, #0

    ; ── 1. Read I-cache INFO ──────────────────────────────────
    RDSYS R2, #ICACHE, #CACHE_INFO
    CMP  R2, R0
    BEQ  fail

    ; ── 2. Copy ram_func from ROM to RAM ──────────────────────
    LA   R2, #ram_func
    LLI  R3, #RAM_DEST
    LA   R4, #ram_func_end

copy:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy

    ; ── 3. Map page 0 cacheable, ROM uncacheable, enable MMU ──
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

    ; ── 4. Enable I-cache ─────────────────────────────────────
    WRSYS R4, #ICACHE, #CACHE_CTRL

    ; ── 5. Call #1: cold miss → I-cache line fills ────────────
    LLI  R5, #10
    LLI  R3, #RAM_DEST
    LA   R13, #after_call1
    JMP  R3

after_call1:
    ; R5 should be (10 + 3) * 2 - 1 = 25
    CMP  R5, #25
    BNE  fail

    ; ── 6. Call #2: should hit in I-cache ─────────────────────
    LLI  R5, #5
    LLI  R3, #RAM_DEST
    LA   R13, #after_call2
    JMP  R3

after_call2:
    ; R5 should be (5 + 3) * 2 - 1 = 15
    CMP  R5, #15
    BNE  fail

    ; ── 7. Invalidate I-cache ─────────────────────────────────
    WRSYS R0, #ICACHE, #CACHE_INVAL

    ; ── 8. Call #3: cache cold again → re-fill from RAM ───────
    LLI  R5, #0
    LLI  R3, #RAM_DEST
    LA   R13, #after_call3
    JMP  R3

after_call3:
    ; R5 should be (0 + 3) * 2 - 1 = 5
    CMP  R5, #5
    BNE  fail

    ; ── Cleanup: disable I-cache and MMU ──────────────────────
    LLI  R4, #0
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #MMU, #MMUCR

    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Position-independent function: R5 = (R5 + 3) * 2 - 1
; No branches (all straight-line), returns via RET (JMP R13).
; 8 instructions = 32 bytes = 2 cache lines (4-word lines).
; ═══════════════════════════════════════════════════════════════
ram_func:
    ADD  R5, #1               ; +1
    ADD  R5, #1               ; +1
    ADD  R5, #1               ; +1 (total: +3)
    ADD  R5, R5               ; *2 (R5 = R5 + R5)
    SUB  R5, #1               ; -1
    NOP                        ; padding to fill second cache line
    NOP
    RET                        ; JMP R13
ram_func_end:
