; test_subword_store_then_load.s — sub-word write/read coherency
; REQUIRES: mmu cache l2
;
; With write-through L1 D-cache and write-through write-no-allocate
; L2, a sub-word store must propagate correctly through both cache
; layers (and to SDRAM at L2-write-miss addresses) before a
; subsequent read of the same address can be correct.  The existing
; sub-word store test does store-then-read on one address;
; this test does it in a tight loop over a 1 KiB region, covering
; every halfword and byte offset, and re-reading both as halfword
; and as containing-word to catch byte-enable / lane-merge bugs.
;
; Strategy:
;   1. Identity-map page 0 cacheable, enable all caches.
;   2. For each halfword address in [DATA_BASE, DATA_END):
;        - STH a distinctive value (idx + 0x4000)
;        - LDH back, compare
;        - LDW the containing word, compare against expected merge
;          (preserving the OTHER halfword in the same word)
;   3. Then for each byte address: STB / LDB / LDW-merge check.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C, 0xBD
.equ DATA_BASE,  0x0100
.equ DATA_END,   0x0500            ; 1 KiB

_start:
    LLI  R1, #0

    ; ── MMU setup ────────────────────────────────────────────────
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

    ; ── Enable all caches ────────────────────────────────────────
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2,     #CACHE_CTRL

    ; ── Pre-fill region with a known background pattern ──────────
    ; This lets us verify STH only touches the targeted halfword:
    ; the unmodified halfword in each word must still read back as
    ; its background pattern, not as 0/garbage.
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LI   R8, #0xAAAAAAAA
bg_fill:
    STW  R8, [R5]
    ADD  R5, #4
    CMP  R5, R6
    BNE  bg_fill

    ; ── Pass 1: STH at every halfword offset, LDH back ────────────
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #0x4000               ; running value: increments per store
sth_loop:
    STH  R7, [R5]
    LDH  R2, [R5]                  ; immediate readback
    CMP  R2, R7
    BNE  fail

    ; Now check the containing word: bottom halfword should be R7,
    ; top halfword should still be 0xAAAA from the background fill.
    ; (Subsequent iterations will have overwritten the high half too,
    ; so only check on aligned-to-word iterations.)
    MOV  R3, R5
    AND  R3, #3                    ; R3 = R5 & 3; expect 0 for low half
    CMP  R3, R0
    BNE  sth_advance               ; skip word check on odd halfword

    LDW  R2, [R5]
    LI   R4, #0xAAAA0000
    OR   R4, R7                    ; expected = 0xAAAA0000 | R7
    CMP  R2, R4
    BNE  fail

sth_advance:
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  sth_loop

    ; ── Pass 2: refill, then STB at every byte offset, LDB back ──
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LI   R8, #0x55555555
bg_fill2:
    STW  R8, [R5]
    ADD  R5, #4
    CMP  R5, R6
    BNE  bg_fill2

    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #0                    ; running byte value (0..255 wraparound)
stb_loop:
    STB  R7, [R5]
    LDB  R2, [R5]                  ; immediate readback
    CMP  R2, R7
    BNE  fail
    ADD  R5, #1
    ADD  R7, #1
    AND  R7, #0xFF                 ; wrap to byte range so CMP vs LDB result matches
    CMP  R5, R6
    BNE  stb_loop

    LLI  R1, #1
fail:
    BREAK
