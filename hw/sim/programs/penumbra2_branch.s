; penumbra2_branch.s — gen2 branch-redirect milestone test.
;
; Exercises the taken-branch PC redirect + 3-bubble front-end flush that closes
; the fetch loop: a forward taken branch, a forward not-taken branch, an
; unconditional branch, and a backward loop whose trip count is only correct if
; every taken redirect steers PC to the target AND discards the wrong-path
; fetches behind the branch. Each taken branch is immediately followed by a
; poison "LLI R1,#0 / BREAK": if the flush fails, the poison BREAK retires and
; the testbench sees R1=0; if the redirect fails, the poison LLI clears R1.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL), the repo-wide hw-test convention.
; Note gen2 has no GPR forwarding: each CMP/ADD that reads a just-written
; register stalls on the scoreboard until the producer commits — the loop body
; deliberately chains R4/R5 RAW deps so the redirect and the scoreboard stall
; interact across iterations.

_start:
    LLI  R1, #0              ; assume FAIL until the final PASS

    ; ── Test 1: forward taken branch skips the poison ───────────
    LLI  R2, #1
    CMP  R2, #1              ; Z=1 → EQ
    BEQ  t1ok                ; taken: must skip the two poison insns
    LLI  R1, #0              ; poison: reached only if PC was not redirected
    BREAK                    ; poison: retires (FAIL) only if the flush leaked
t1ok:

    ; ── Test 2: forward not-taken branch falls through ──────────
    LLI  R3, #2
    CMP  R3, #5              ; Z=0 → not EQ
    BEQ  fail                ; not taken; falling through is correct

    ; ── Test 3: unconditional branch over the poison ────────────
    B    t3ok
    LLI  R1, #0              ; poison
    BREAK                    ; poison
t3ok:

    ; ── Test 4: backward loop, 4 iterations, sum 4+3+2+1 = 10 ────
    LLI  R4, #0              ; accumulator
    LLI  R5, #4              ; trip counter
loop:
    ADD  R4, R5             ; R4 += R5   (RAW on R4 across iterations)
    SUB  R5, #1             ; R5 -= 1    (Format L DEC: sets Z when R5 hits 0)
    BNE  loop               ; backward redirect while R5 != 0
    CMP  R4, #10            ; loop must have summed to 10
    BNE  fail

    ; ── All checks passed ───────────────────────────────────────
    LLI  R1, #1             ; PASS
fail:
    BREAK
