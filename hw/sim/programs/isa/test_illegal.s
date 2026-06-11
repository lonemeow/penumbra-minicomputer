; test_illegal.s — test illegal instruction trap
; REQUIRES: wrspr
;
; Verifies:
;   1. Executing an undefined instruction traps to vector 7 (VEC_ILLEGAL)
;   2. Handler can skip the faulting instruction and resume execution
;   3. Multiple different illegal instructions all trap correctly

; ── Main test ────────────────────────────────────────────
_start:
    LLI R2, #0                  ; R2 = handler call count

    ; ── Install vector table in RAM ─────────────────────────
    LA   R3, #illegal_handler
    STW  R3, [R0 + #0x1C]      ; vector[7] = illegal instruction (VEC_ILLEGAL)

    ; Test 1: reserved Format R op=12 (single-cycle ALU gap, op 12-15)
    ; Encoding: [00][01100][Rd=0000][Rs=0000][F=0][spare=0x0000] = 0x18000000
    .word 0x18000000

    ; Handler should have incremented R2 to 1
    CMP  R2, #1
    BNE fail

    ; Test 2: reserved Format R op=20 (peer-unit gap, op 20-22).
    ; [00][10100][Rd=0000][Rs=0000][F=0][spare=0] = 0x28000000
    .word 0x28000000

    ; Handler should have incremented R2 to 2
    CMP  R2, #2
    BNE fail

    ; Pass
    LLI R1, #1
fail:
    BREAK

; ── Illegal instruction handler ──────────────────────────
;
; Must skip the faulting instruction and resume execution.
; WRSPR EPC advances past the faulting instruction, then ERET
; returns via the updated EPC (ESR is unchanged).
;
illegal_handler:
    ADD   R2, #1                  ; count handler invocations
    RDSPR R9, EPC
    ADD   R9, #4
    WRSPR EPC, R9
    ERET
