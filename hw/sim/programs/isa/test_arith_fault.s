; test_arith_fault.s — divide-by-zero traps to VEC_ARITH (vector 10, 0x28)
; REQUIRES: wrspr
;
; Verifies:
;   1. DIVU by zero traps to VEC_ARITH
;   2. DIV  (signed) by zero traps too
;   3. The handler can skip the faulting instruction and resume
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R2, #0                 ; handler invocation count

    ; install VEC_ARITH handler at vector[10] = 10*4 = 0x28
    LA   R3, #arith_handler
    STW  R3, [R0 + #0x28]

    ; ── unsigned divide by zero ─────────────────────────────
    LLI  R4, #10
    LLI  R5, #0
    DIVU R4, R5                 ; traps → VEC_ARITH; handler skips it
    CMP  R2, #1
    BNE  fail

    ; ── signed divide by zero ───────────────────────────────
    LLI  R6, #7
    LLI  R7, #0
    DIV  R6, R7                 ; traps again
    CMP  R2, #2
    BNE  fail

    ; ── a normal divide still works afterwards ──────────────
    LLI  R4, #20
    LLI  R5, #6
    DIVU R4, R5                 ; 20 / 6 = 3
    CMP  R4, #3
    BNE  fail

    LLI  R1, #1
    BREAK
fail:
    LLI  R1, #0
    BREAK

; ── VEC_ARITH handler: count, skip the faulting instruction, resume ──
arith_handler:
    ADD   R2, #1
    RDSPR R9, EPC
    ADD   R9, #4                ; advance past the faulting DIV
    WRSPR EPC, R9
    ERET
