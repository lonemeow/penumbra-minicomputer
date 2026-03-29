; test_data.s — Verify .byte, .word, and .asciz assembler directives
;
; Tests sub-word loads from ROM data sections to confirm correct
; little-endian byte packing and address calculation.
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: .asciz — read bytes from a string ────────────
    LA   R4, #greeting
    LDB  R2, [R4 + #0]        ; 'H' = 0x48
    CMP  R2, #0x48
    BNE  fail

    LDB  R2, [R4 + #1]        ; 'i' = 0x69
    CMP  R2, #0x69
    BNE  fail

    LDB  R2, [R4 + #2]        ; '\n' = 0x0A
    CMP  R2, #0x0A
    BNE  fail

    LDB  R2, [R4 + #3]        ; null terminator = 0x00
    CMP  R2, #0
    BNE  fail

    ; ── Test 2: .byte — read individual bytes ────────────────
    LA   R4, #test_bytes
    LDB  R2, [R4 + #0]        ; 0xDE
    CMP  R2, #0xDE
    BNE  fail

    LDB  R2, [R4 + #1]        ; 0xAD
    CMP  R2, #0xAD
    BNE  fail

    LDB  R2, [R4 + #2]        ; 0xBE
    CMP  R2, #0xBE
    BNE  fail

    LDB  R2, [R4 + #3]        ; 0xEF
    CMP  R2, #0xEF
    BNE  fail

    ; Second word of .byte
    LDB  R2, [R4 + #4]        ; 0x42
    CMP  R2, #0x42
    BNE  fail

    ; ── Test 3: .word with multiple values ───────────────────
    LA   R4, #test_words
    LDW  R2, [R4 + #0]
    LI   R3, #0x12345678
    CMP  R2, R3
    BNE  fail

    LDW  R2, [R4 + #4]
    LI   R3, #0xCAFEBABE
    CMP  R2, R3
    BNE  fail

    ; ── Test 4: .asciz escape sequences ──────────────────────
    LA   R4, #escapes
    LDB  R2, [R4 + #0]        ; '\t' = 0x09
    CMP  R2, #0x09
    BNE  fail

    LDB  R2, [R4 + #1]        ; '\\' = 0x5C
    CMP  R2, #0x5C
    BNE  fail

    LDB  R2, [R4 + #2]        ; '\r' = 0x0D
    CMP  R2, #0x0D
    BNE  fail

    ; All tests passed
    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Test data (lives in ROM after the code)
; ═══════════════════════════════════════════════════════════════

greeting:
    .asciz "Hi\n"

test_bytes:
    .byte 0xDE, 0xAD, 0xBE, 0xEF, 0x42

test_words:
    .word 0x12345678, 0xCAFEBABE

escapes:
    .asciz "\t\\\r"
