; test_subword_store.s — Verify STB/STH sub-word stores
;
; Writes known word patterns, then overwrites individual bytes and
; halfwords, reading back the full word to verify only the targeted
; bytes changed.
;
; Result: R1=1 PASS, R1=0 FAIL

.org 0x00
    B    start

.org 0x40
start:
    LLI  R1, #0
    LLI  R9, #0x200         ; base address (well past program code)

    ; ══════════════════════════════════════════════════════
    ; Test 1: STB at byte offset 0
    ; ══════════════════════════════════════════════════════
    LLI  R8, #0xFFFF
    LUI  R8, #0xFFFF        ; R8 = 0xFFFFFFFF
    STW  R8, [R9]            ; fill with FF

    LLI  R2, #0x42
    STB  R2, [R9]            ; store 0x42 at byte 0

    LDW  R3, [R9]
    LLI  R4, #0xFF42
    LUI  R4, #0xFFFF        ; expected: 0xFFFFFF42
    CMP  R3, R4
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 2: STB at byte offset 2
    ; ══════════════════════════════════════════════════════
    STW  R8, [R9]            ; refill with FF
    LLI  R2, #0xAB
    STB  R2, [R9 + #2]      ; store 0xAB at byte 2

    LDW  R3, [R9]
    LLI  R4, #0xFFFF
    LUI  R4, #0xFFAB        ; expected: 0xFFABFFFF
    CMP  R3, R4
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 3: STB at byte offset 1 and 3
    ; ══════════════════════════════════════════════════════
    LLI  R8, #0
    STW  R8, [R9]            ; clear to 0x00000000

    LLI  R2, #0x11
    STB  R2, [R9 + #1]      ; byte 1 = 0x11
    LLI  R2, #0x33
    STB  R2, [R9 + #3]      ; byte 3 = 0x33

    LDW  R3, [R9]
    LLI  R4, #0x1100
    LUI  R4, #0x3300        ; expected: 0x33001100
    CMP  R3, R4
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 4: STH at halfword offset 0
    ; ══════════════════════════════════════════════════════
    STW  R8, [R9]            ; clear to 0 (R8=0 from above)
    LLI  R2, #0xBEEF
    STH  R2, [R9]            ; store 0xBEEF at lower half

    LDW  R3, [R9]
    CMP  R3, #0xBEEF        ; expected: 0x0000BEEF
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 5: STH at halfword offset 2
    ; ══════════════════════════════════════════════════════
    LLI  R8, #0xFFFF
    LUI  R8, #0xFFFF        ; R8 = 0xFFFFFFFF
    STW  R8, [R9]            ; fill with FF

    LLI  R2, #0x1234
    STH  R2, [R9 + #2]      ; store 0x1234 at upper half

    LDW  R3, [R9]
    LLI  R4, #0xFFFF
    LUI  R4, #0x1234        ; expected: 0x1234FFFF
    CMP  R3, R4
    BNE  fail

    ; All passed
    LLI  R1, #1
fail:
    BREAK
