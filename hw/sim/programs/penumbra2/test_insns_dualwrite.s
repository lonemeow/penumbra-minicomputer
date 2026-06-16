; test_insns_dualwrite.s — INSNS_RETIRED counts a dual-write divmul once.
;
; A 3-operand MULU writes both Rd (low half) and Rhi (high half); WB sequences
; the two register writes over two cycles, holding the MEM/WB slot valid across
; both. INSNS_RETIRED must count that instruction ONCE, not once per write
; cycle — o_insn_committed excludes the dual-write continuation cycle.
;
; Checked differentially against a single-write multiply: two windows of
; identical [RDSYS, multiply, RDSYS] shape — one a dual-write MULU, one a
; single-write MUL — must report equal INSNS deltas. The pipeline/bracketing
; skew is identical in both windows, so it cancels; only the multiply's retire
; count differs if the dual write is miscounted (the bug counted MULU as +2).
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

_start:
    LLI  R1, #0               ; assume FAIL
    LLI  R5, #0x0007          ; multiplicand
    LLI  R6, #0x0003          ; multiplier

    ; ── Window A: dual-write MULU (writes R5 low, R7 high) ──
    RDSYS R10, #CPU, #CPU_INSNS_RETIRED
    MULU R5, R6, R7
    RDSYS R11, #CPU, #CPU_INSNS_RETIRED
    SUB  R11, R10             ; R11 = INSNS delta over the dual-write window

    LLI  R5, #0x0007          ; restore operands (MULU overwrote R5)
    LLI  R6, #0x0003

    ; ── Window B: single-write MUL (writes R5 only) ──
    RDSYS R12, #CPU, #CPU_INSNS_RETIRED
    MUL  R5, R6
    RDSYS R13, #CPU, #CPU_INSNS_RETIRED
    SUB  R13, R12             ; R13 = INSNS delta over the single-write window

    ; Equal deltas ⇒ the dual write counted as exactly one instruction.
    CMP  R11, R13
    BNE  fail

    LLI  R1, #1               ; PASS
fail:
    BREAK
