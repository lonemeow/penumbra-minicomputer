; test_andi.s — Test AND-immediate and TEST-immediate instructions
;
; Convention: R1 = 1 on pass, R1 = 0 on fail.

; ── Boot preamble ────────────────────────────────────────────
        LA   LR, #done
        B    tests

done:
        ; If we get here, all tests passed
        LLI  R1, #1            ; PASS
        BREAK
fail:
        LLI  R1, #0            ; FAIL
        BREAK

; ── tests ────────────────────────────────────────────────────
tests:

        ; --- ANDi: basic mask ---
        LLI  R2, #0xFF37       ; R2 = 0x0000FF37
        AND  R2, #0x00FF       ; R2 = R2 & 0x00FF = 0x0037
        CMP  R2, #0x0037
        BNE  fail

        ; --- ANDi: mask to zero sets Z flag ---
        LLI  R3, #0xFF00
        AND  R3, #0x00FF       ; R3 = 0x0000, Z=1
        BNE  fail              ; should be zero

        ; --- ANDi: preserve high bits not in mask ---
        LI   R4, #0xDEAD5678   ; full 32-bit value
        AND  R4, #0xFFFF       ; keep lower 16 bits: 0x00005678
        LI   R5, #0x00005678
        CMP  R4, R5
        BNE  fail

        ; --- TESTi: flags only, no write ---
        LLI  R6, #0x0040       ; bit 6 set
        TEST R6, #0x0040       ; Z=0 (bit 6 is set)
        BEQ  fail              ; should NOT be zero

        ; --- TESTi: no match → Z=1 ---
        LLI  R7, #0x0040       ; bit 6 set
        TEST R7, #0x0080       ; Z=1 (bit 7 not set)
        BNE  fail              ; should be zero

        ; --- TESTi: verify register unchanged ---
        LLI  R8, #0xABCD
        TEST R8, #0x00FF       ; flags only
        CMP  R8, #0xABCD       ; R8 should still be 0xABCD
        BNE  fail

        ; --- ANDi: extract UART status bits (real-world use case) ---
        ; Simulates: LDW R2, [UART_LSR]; TEST R2, #0x20 (THRE check)
        LLI  R9, #0x0060       ; fake LSR: THRE=1, TEMT=1
        TEST R9, #0x0020       ; check THRE bit
        BEQ  fail              ; THRE should be set

        ; All passed — return via LR
        JMP  R13
