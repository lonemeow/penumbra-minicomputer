; test_adc.s — test add-with-carry and subtract-with-borrow
;
; Verifies:
;   1. ADC adds carry flag into result
;   2. SBC subtracts with borrow (ARM convention: A - B - ~C)
;   3. 64-bit addition using ADD+ADC
;   4. 64-bit subtraction using SUB+SBC
;   5. Carry flag propagation across multi-word ops

_start:
    ; ── Test 1: ADC with carry clear ──
    ; Set C=0 by doing 0+0 (ADD sets C=0)
    ADD R0, R0
    LLI R1, #10
    LLI R2, #20
    ADC R1, R2          ; R1 = 10 + 20 + 0 = 30
    CMP R1, #30
    BNE fail

    ; ── Test 2: ADC with carry set ──
    ; Set C=1 by doing 0xFFFFFFFF + 1
    LLIS R3, #-1        ; R3 = 0xFFFFFFFF
    LLI R4, #1
    ADD R3, R4          ; R3 = 0, C=1
    LLI R1, #10
    LLI R2, #20
    ADC R1, R2          ; R1 = 10 + 20 + 1 = 31
    CMP R1, #31
    BNE fail

    ; ── Test 3: SBC with carry set (no borrow) ──
    ; Set C=1
    LLIS R3, #-1
    LLI R4, #1
    ADD R3, R4          ; C=1
    LLI R1, #50
    LLI R2, #20
    SBC R1, R2          ; R1 = 50 - 20 - 0 = 30 (C=1 means no borrow)
    CMP R1, #30
    BNE fail

    ; ── Test 4: SBC with carry clear (borrow) ──
    ; Set C=0
    ADD R0, R0          ; C=0
    LLI R1, #50
    LLI R2, #20
    SBC R1, R2          ; R1 = 50 - 20 - 1 = 29 (C=0 means borrow)
    CMP R1, #29
    BNE fail

    ; ── Test 5: 64-bit addition ──
    ; 0x00000001_FFFFFFFE + 0x00000000_00000003 = 0x00000002_00000001
    ; Low: 0xFFFFFFFE + 3 = 0x00000001 with carry
    ; High: 1 + 0 + carry = 2
    LLI R1, #0
    LUI R1, #0
    LLIS R1, #-2        ; R1 = 0xFFFFFFFE (low)
    LLI R2, #1          ; R2 = 1 (high)
    LLI R3, #3          ; R3 = 3 (addend low)
    LLI R4, #0          ; R4 = 0 (addend high)
    ADD R1, R3           ; low = 0xFFFFFFFE + 3 = 1, C=1
    ADC R2, R4           ; high = 1 + 0 + 1 = 2
    CMP R1, #1
    BNE fail
    CMP R2, #2
    BNE fail

    ; ── Test 6: 64-bit subtraction ──
    ; 0x00000002_00000001 - 0x00000001_00000003 = 0x00000000_FFFFFFFE
    ; Low: 1 - 3 = 0xFFFFFFFE, borrows (C=0)
    ; High: 2 - 1 - borrow = 0
    LLI R1, #1          ; R1 = 1 (low)
    LLI R2, #2          ; R2 = 2 (high)
    LLI R3, #3          ; R3 = 3 (subtrahend low)
    LLI R4, #1          ; R4 = 1 (subtrahend high)
    SUB R1, R3           ; low = 1 - 3 = 0xFFFFFFFE, C=0 (borrow)
    SBC R2, R4           ; high = 2 - 1 - 1 = 0
    LLIS R5, #-2        ; R5 = 0xFFFFFFFE
    CMP R1, R5
    BNE fail
    CMP R2, #0
    BNE fail

    ; All passed
    LLI R1, #1
fail:
    BREAK
