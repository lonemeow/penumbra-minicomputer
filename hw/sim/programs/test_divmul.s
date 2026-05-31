; test_divmul.s — hardware MUL/MULU/DIV/DIVU via the divmul peer unit
;
; Exercises the 2-operand forms (low result → Rd: product low / quotient):
;   - signed MUL (positive and negative)
;   - MUL does not clobber its Rs source
;   - MULU with a high-bit product
;   - DIVU quotient
;   - signed DIV quotient (negative)
;   - DIVU with a zero quotient
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    ; ── signed MUL: 6 * 7 = 42 ──────────────────────────────
    LLI  R2, #6
    LLI  R3, #7
    MUL  R2, R3               ; R2 = 42
    CMP  R2, #42
    BNE  fail

    ; ── MUL must not clobber Rs ─────────────────────────────
    CMP  R3, #7
    BNE  fail

    ; ── signed MUL negative: -3 * 5 = -15 ───────────────────
    LLIS R2, #-3              ; 0xFFFFFFFD
    LLI  R3, #5
    MUL  R2, R3               ; R2 = -15 = 0xFFFFFFF1
    LLIS R4, #-15
    CMP  R2, R4
    BNE  fail

    ; ── MULU high product: 0x8000 * 2 = 0x10000 ─────────────
    LLI  R2, #0x8000
    LLI  R3, #2
    MULU R2, R3               ; R2 = 0x00010000
    LLI  R4, #0               ; LUI merges (preserves low 16), so clear first
    LUI  R4, #1               ; R4 = 0x00010000
    CMP  R2, R4
    BNE  fail

    ; ── DIVU: 100 / 7 = 14 ──────────────────────────────────
    LLI  R2, #100
    LLI  R3, #7
    DIVU R2, R3               ; R2 = 14
    CMP  R2, #14
    BNE  fail

    ; ── signed DIV negative: -17 / 5 = -3 ───────────────────
    LLIS R2, #-17             ; 0xFFFFFFEF
    LLI  R3, #5
    DIV  R2, R3               ; R2 = -3 = 0xFFFFFFFD
    LLIS R4, #-3
    CMP  R2, R4
    BNE  fail

    ; ── DIVU zero quotient: 3 / 5 = 0 ───────────────────────
    LLI  R2, #3
    LLI  R3, #5
    DIVU R2, R3               ; R2 = 0
    CMP  R2, #0
    BNE  fail

    ; all checks passed
    LLI  R1, #1
    BREAK

fail:
    LLI  R1, #0
    BREAK
