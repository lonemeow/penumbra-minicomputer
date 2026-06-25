; test_btfn.s — gen2.5 direct-branch direction-correctness test.
;
; Direct branches are predicted at fetch by the BTB and confirmed or corrected
; in EX. This exercises that path end-to-end through ID->spine->EX->core —
; catching wiring errors the EX unit test cannot, since that drives the stage
; in isolation. Architectural results are identical with or without prediction,
; so every check below is result-based: a recovery bug shows up as a wrong
; value or a retired poison BREAK.
;
; Cases:
;   1. backward loop  — predicted taken ×4 (correct), then a predicted-taken/
;                       actually-not-taken misprediction on the exit; the sum is
;                       only right if EX recovers to the fall-through.
;   2. forward Bcc taken — predicted not-taken, actually taken: EX redirects to
;                       the target (mispredict the other way).
;   3. unconditional B  — predicted taken; must skip the poison.
;   4. fault-shadowed predicted branch — a predicted-taken branch immediately
;                       behind a faulting load must be squashed by the precise
;                       exception; the fault must reach the handler, and the
;                       branch's speculative redirect must never leak.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL); run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                 ; assume FAIL until the handler proves the squash

    ; ── Install the alignment-fault handler (vector table in low RAM) ──
    LA   R2, fault_handler
    LLI  R3, #0x20             ; VEC_ALIGN (8) << 2 = table slot 0x20
    STW  R2, [R3]

    ; ── Test 1: backward loop, 4 iterations, sum 4+3+2+1 = 10 ────────
    ; Each BNE is a backward branch (BTFN predicts taken): taken correctly ×4,
    ; then the exit is a predicted-taken/not-taken misprediction. The sum is 10
    ; only if EX recovers to the fall-through exactly once, on the exit.
    LLI  R4, #0                ; accumulator
    LLI  R5, #4                ; trip counter
loop:
    ADD  R4, R5
    SUB  R5, #1                ; sets Z when R5 reaches 0
    BNE  loop                  ; backward, predicted taken
    CMP  R4, #10
    BNE  fail                  ; wrong trip count ⇒ mispredict recovery is broken

    ; ── Test 2: forward taken branch (predicted not-taken, actually taken) ──
    LLI  R2, #1
    CMP  R2, #1                ; Z=1 → EQ
    BEQ  t2ok                  ; forward + taken ⇒ EX redirects to the target
    LLI  R1, #0                ; poison: redirect failed
    BREAK
t2ok:

    ; ── Test 3: unconditional branch (predicted taken) over the poison ──
    B    t3ok
    LLI  R1, #0                ; poison
    BREAK
t3ok:

    ; ── Test 4: a predicted branch in a fault's shadow must be squashed ──
    ; The misaligned load is older and faults at WB; the unconditional B behind
    ; it is predicted taken, so ID has already steered fetch to backward_target.
    ; The precise exception must flush that speculation and reach the handler;
    ; if the branch's redirect leaks, control lands on the poison at
    ; backward_target instead.
    LLI  R4, #0x200            ; aligned base for the misaligned load
    B    trigger               ; (forward uncond, predicted taken) skip the target

backward_target:
    LLI  R1, #0                ; poison: the shadowed branch's redirect leaked
    BREAK

trigger:
    LDW  R5, [R4 + #1]         ; OLDER: EA = 0x201, word-misaligned → VEC_ALIGN
    B    backward_target       ; YOUNGER: predicted-taken branch in the fault's
                               ;          shadow — must be squashed, never taken
    LLI  R1, #0                ; poison: fall-through is also wrong
    BREAK

fail:
    BREAK                       ; R1 still 0 → FAIL

fault_handler:
    LLI  R1, #1                ; PASS — the fault took precedence and the shadowed
    BREAK                      ;        branch neither redirected nor leaked
