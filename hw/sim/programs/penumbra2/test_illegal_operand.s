; test_illegal_operand.s — gen2: invalid operand forms trap to VEC_ILLEGAL.
; REQUIRES: wrspr
;
; An instruction that names R15/PC as a destination, or an undefined SPR
; number, is not a silent no-op — it is an illegal instruction that raises
; VEC_ILLEGAL for software to handle. Such forms only arise from malformed or
; speculatively-fetched wrong-path bytes; this checks the architectural trap
; behaviour end-to-end (decode -> fault -> handler -> resume). The handler skips
; the faulting instruction and continues. Self-checks into R1 (1 = PASS,
; 0 = FAIL); run via tb_penumbra2_prog.
;
; (Lives in the gen2 suite, not the shared isa/ conformance suite: the gen1
; core and the ISS still silently ignore these forms, so the cross-generation
; suite cannot require the trap until they are aligned.)

_start:
    LLI  R1, #0                 ; assume FAIL
    LLI  R2, #0                 ; handler invocation count

    LA   R3, #illegal_handler
    STW  R3, [R0 + #0x1C]       ; vector[7] = VEC_ILLEGAL handler

    ; ── Test 1: R15/PC as a destination ─────────────────────────
    ; MOV R15, R0 = [00][01000][Rd=1111][Rs=0000][F=0][...] = 0x11E00000
    .word 0x11E00000
    CMP  R2, #1
    BNE  fail

    ; ── Test 2: undefined SPR number (defined are 0–7) ──────────
    ; RDSPR R5, #9 = [00][11111][Rd=0101][Rs=0000][F=0][9][...] = 0x3EA09000
    .word 0x3EA09000
    CMP  R2, #2
    BNE  fail

    LLI  R1, #1                 ; both forms trapped and resumed → PASS
fail:
    BREAK

; ── Illegal-instruction handler ─────────────────────────────────
; Skip the faulting instruction (EPC += 4) and return via ERET.
illegal_handler:
    ADD   R2, #1
    RDSPR R9, EPC
    ADD   R9, #4
    WRSPR EPC, R9
    ERET
