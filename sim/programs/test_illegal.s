; test_illegal.s — test illegal instruction trap
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

    ; Test 1: undefined Format L op=7 (reserved)
    ; Encoding: [01][111][Rd=0000][spare=0000000][imm16=0x0000] = 0x7C000000
    .word 0x7C000000

    ; Handler should have incremented R2 to 1
    CMP  R2, #1
    BNE fail

    ; Test 2: MUL R3, R4 — defined in ISA but no microcode (sentinel)
    MUL R3, R4

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
