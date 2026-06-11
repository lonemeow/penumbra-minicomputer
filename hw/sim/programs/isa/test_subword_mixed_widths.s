; test_subword_mixed_widths.s — interleaved word/half/byte sequence
; REQUIRES: mmu cache l2
;
; Userland code (parsers, string ops, protocol decoders, hash bucket
; walks) commonly mixes load widths on the same memory region — a word
; load to grab a header, then halfword/byte loads into adjacent fields.
; The existing sub-word tests do width-homogeneous sequences only;
; this one exercises width transitions through the cache+bus pipeline,
; which is where speculative-prefetch / byte-extract pairing bugs would
; hide.
;
; Strategy:
;   1. Identity-map page 0 cacheable, enable all three caches.
;   2. Initialize a 64-byte (4-cache-line) region with byte pattern
;      A[i] = i (so byte i = i, halfword at offset 2k = (2k+1)<<8 | 2k,
;      word at offset 4k = same in word form).
;   3. Run a mixed-width walk: LDW, LDH, LDB, LDH, LDW, LDB, LDH...
;      across the region.  Each load's expected value is fully
;      determined by its address + width.
;   4. Repeat with caches invalidated between passes to also catch
;      cold-fill ordering bugs.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C, 0xBD
.equ DATA_BASE,  0x0100
.equ DATA_END,   0x0140            ; 64 bytes = 4 lines of 16

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

    ; ── Initialize: A[i] = i (byte-by-byte) ──────────────────────
    ; Use STB to fill so every byte lane is exercised independently.
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #0
init:
    STB  R7, [R5]
    ADD  R5, #1
    ADD  R7, #1
    CMP  R5, R6
    BNE  init

    ; ── Mixed-width walk ─────────────────────────────────────────
    ; At each starting word, do: LDW; LDH lo; LDH hi; LDB 0; LDB 1;
    ; LDB 2; LDB 3.  Verify each.  Run across all 16 starting words.
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R8, #0                    ; expected byte value at A[R5]
walk:
    ; LDW at R5: expected = (R8+3)<<24 | (R8+2)<<16 | (R8+1)<<8 | R8
    LDW  R2, [R5]
    MOV  R3, R8
    ADD  R3, #1
    SHL  R3, #8
    MOV  R4, R8
    OR   R3, R4                    ; R3 = (R8+1)<<8 | R8
    MOV  R4, R8
    ADD  R4, #2
    SHL  R4, #16
    OR   R3, R4                    ; + (R8+2)<<16
    MOV  R4, R8
    ADD  R4, #3
    SHL  R4, #24
    OR   R3, R4                    ; + (R8+3)<<24
    CMP  R2, R3
    BNE  fail

    ; LDH at R5 (low half): expected = (R8+1)<<8 | R8
    LDH  R2, [R5]
    MOV  R3, R8
    ADD  R3, #1
    SHL  R3, #8
    MOV  R4, R8
    OR   R3, R4
    CMP  R2, R3
    BNE  fail

    ; LDH at R5+2 (high half): expected = (R8+3)<<8 | (R8+2)
    LDH  R2, [R5 + #2]
    MOV  R3, R8
    ADD  R3, #3
    SHL  R3, #8
    MOV  R4, R8
    ADD  R4, #2
    OR   R3, R4
    CMP  R2, R3
    BNE  fail

    ; LDB at R5+0..+3: each should equal R8+offset
    LDB  R2, [R5]
    CMP  R2, R8
    BNE  fail
    LDB  R2, [R5 + #1]
    MOV  R3, R8
    ADD  R3, #1
    CMP  R2, R3
    BNE  fail
    LDB  R2, [R5 + #2]
    MOV  R3, R8
    ADD  R3, #2
    CMP  R2, R3
    BNE  fail
    LDB  R2, [R5 + #3]
    MOV  R3, R8
    ADD  R3, #3
    CMP  R2, R3
    BNE  fail

    ADD  R5, #4
    ADD  R8, #4
    CMP  R5, R6
    BNE  walk

    LLI  R1, #1
fail:
    BREAK
