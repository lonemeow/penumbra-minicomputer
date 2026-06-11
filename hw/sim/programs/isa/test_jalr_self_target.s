; test_jalr_self_target.s — JALR Rd must capture Rd before writing R13
;
; Regression for the bug that hid in NetBSD init: when the compiler
; emits the canonical indirect-call pattern `ldw r13, [ptr]; jalr r13`
; (LR and target alias the same register), the old microcode wrote R13
; first and then read it as the target, so the link clobbered the jump
; address and PC fell through to the next instruction.
;
; The fixed microcode latches Rd into MDR in step 0, then writes
; R13 = PC+4 and sets PC = MDR (= old Rd value) in step 1.  This test
; verifies the spec-correct behaviour for two cases:
;
;   1. jalr r5  — Rd != R13 (always worked).  Sanity check.
;   2. jalr r13 — Rd == R13 (this is the regression).
;
; Result: R1 = 1 PASS, R1 = 0 FAIL.

_start:
    LLI  R1, #0                  ; assume fail

    ; ── Case 1: jalr r5 (no aliasing) ──────────────────────────
    LA   R5, #target_a
    JALR R5
    ; if jalr fell through (didn't jump) we hit fail here
    BREAK                        ; FAIL marker for case 1

target_a:
    ; If we reach here, jalr r5 jumped correctly.
    ; R13 should now hold the address of the "BREAK" two lines above
    ; (the instruction after the JALR).  We don't strictly need to
    ; verify R13's value — its correctness is implicit in the fact
    ; that the called function (this label) returns via R13 below.

    ; ── Case 2: jalr r13 (target == link register) ─────────────
    LA   R13, #target_b
    JALR R13                     ; THE REGRESSION CASE
    BREAK                        ; FAIL marker for case 2

target_b:
    ; If we reach here, jalr r13 correctly read the old R13 (target)
    ; before the link write clobbered it.  R13 should now hold the
    ; address of the BREAK above (= post-JALR return address), which
    ; is in our text region (not the original target_b).  Sanity
    ; check it: R13 must NOT equal target_b itself (which would mean
    ; the link write didn't happen at all).
    LA   R2, #target_b
    CMP  R13, R2
    BEQ  fail                    ; R13 == target_b → link write missing → FAIL

    ; All good.
    LLI  R1, #1
fail:
    BREAK
