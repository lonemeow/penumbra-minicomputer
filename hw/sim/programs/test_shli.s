; Test immediate shift instructions (SHLi, SHRi, SARi)
_start:
    ; ── SHL immediate: basic ─────────────────────────────────────
    LLI  R2, #1
    SHL  R2, #8        ; 1 << 8 = 0x100
    LLI  R3, #0x100
    CMP  R2, R3
    BNE  fail

    ; ── SHL immediate: shift by 0 (identity) ─────────────────────
    LLI  R2, #0x1234
    SHL  R2, #0        ; unchanged
    CMP  R2, R3        ; R3 still 0x100, R2 should be 0x1234
    BEQ  fail          ; they must NOT be equal
    LLI  R3, #0x1234
    CMP  R2, R3
    BNE  fail

    ; ── SHL immediate: shift by 31 ───────────────────────────────
    LLI  R2, #1
    SHL  R2, #31       ; 1 << 31 = 0x80000000
    LLI  R3, #0
    CMP  R2, R3
    BGE  fail          ; result is negative (MSB set)

    ; ── SHR immediate: basic ─────────────────────────────────────
    LLI  R4, #0x8000
    SHR  R4, #4        ; 0x8000 >> 4 = 0x0800
    LLI  R5, #0x0800
    CMP  R4, R5
    BNE  fail

    ; ── SHR immediate: shift by 0 ───────────────────────────────
    LLI  R4, #0xABCD
    SHR  R4, #0        ; unchanged
    LLI  R5, #0xABCD
    CMP  R4, R5
    BNE  fail

    ; ── SHR immediate: logical (no sign extension) ───────────────
    LLIS R4, #-1       ; R4 = 0xFFFFFFFF
    SHR  R4, #16       ; logical: 0x0000FFFF
    LLI  R5, #0xFFFF
    CMP  R4, R5
    BNE  fail

    ; ── SAR immediate: sign extension ────────────────────────────
    LLIS R6, #-128     ; R6 = 0xFFFFFF80
    SAR  R6, #4        ; arithmetic: 0xFFFFFFF8
    LLIS R7, #-8       ; R7 = 0xFFFFFFF8
    CMP  R6, R7
    BNE  fail

    ; ── SAR immediate: positive stays positive ───────────────────
    LLI  R6, #0x7F00
    SAR  R6, #8        ; 0x7F00 >> 8 = 0x007F
    LLI  R7, #0x007F
    CMP  R6, R7
    BNE  fail

    ; ── SAR immediate: shift by 31 on negative → all 1s ─────────
    LLIS R6, #-1       ; R6 = 0xFFFFFFFF
    SAR  R6, #31       ; all sign bits → 0xFFFFFFFF
    LLIS R7, #-1
    CMP  R6, R7
    BNE  fail

    ; ── Register-form shifts still work ──────────────────────────
    LLI  R2, #1
    LLI  R3, #16
    SHL  R2, R3        ; 1 << 16 = 0x10000
    LLI  R4, #0
    LUI  R4, #1        ; R4 = 0x10000
    CMP  R2, R4
    BNE  fail

    LLI  R1, #1        ; PASS
    BREAK

fail:
    LLI  R1, #0        ; FAIL
    BREAK
