; test_divmul_hi.s — high-half writeback (Rdh): remainder and 64-bit product
;
; Uses the 3-operand form `OP Rd, Rs, Rdh` so the high result (remainder /
; product high) lands in Rdh. The Rdh register is zeroed before DIVU so the
; (not-yet-wired) narrowing dividend-high input reads 0 on the ISS too.
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    ; ── DIVU remainder: 17 / 5 = 3 r 2 ──────────────────────
    LLI  R4, #0                ; Rdh-input = 0 (plain 32/32)
    LLI  R2, #17
    LLI  R3, #5
    DIVU R2, R3, R4            ; R2 = 3 (quotient), R4 = 2 (remainder)
    CMP  R2, #3
    BNE  fail
    CMP  R4, #2
    BNE  fail

    ; ── signed DIV remainder: -17 / 5 = -3 r -2 ─────────────
    LLIS R2, #-17
    LLI  R3, #5
    DIV  R2, R3, R4           ; R2 = -3, R4 = -2  (signed DIV ignores Rdh input)
    LLIS R5, #-3
    CMP  R2, R5
    BNE  fail
    LLIS R5, #-2
    CMP  R4, R5
    BNE  fail

    ; ── 64-bit unsigned product: 0x10000 * 0x10000 = 0x1_0000_0000 ──
    LLI  R2, #0
    LUI  R2, #1               ; 0x00010000
    LLI  R3, #0
    LUI  R3, #1               ; 0x00010000
    MULU R2, R3, R4           ; R2 = lo = 0, R4 = hi = 1
    CMP  R2, #0
    BNE  fail
    CMP  R4, #1
    BNE  fail

    ; ── signed product high half: -2 * 3 = -6 → 0xFFFF_FFFF_FFFF_FFFA ──
    LLIS R2, #-2
    LLI  R3, #3
    MUL  R2, R3, R4          ; R2 = lo = -6 (0xFFFFFFFA), R4 = hi = -1
    LLIS R5, #-6
    CMP  R2, R5
    BNE  fail
    LLIS R5, #-1
    CMP  R4, R5
    BNE  fail

    LLI  R1, #1
    BREAK
fail:
    LLI  R1, #0
    BREAK
