; test_btb.s — gen2.5 branch-target-buffer integration test.
;
; Exercises the fetch-time BTB end to end: the IF1 lookup, the predicted-taken
; tag through IF2 + the fetch buffer, the ID merge (tag set / ID redirect
; suppressed on a hit), and EX verify + train. Like BTFN/RAS the BTB is a
; performance hint guarded by EX, so architectural results are identical with or
; without it and every check is result-based. What this reaches that the BTFN
; test cannot is the BTB's *stateful* behaviour — an entry is allocated on a
; taken resolve, hit on the next fetch of the same PC, and invalidated on a
; not-taken resolve — so a corrupted tag, redirect, or train shows up as a wrong
; value or a poison BREAK.
;
; Cases:
;   1. a long backward loop — the loop branch is a BTB miss on iteration 1
;      (BTFN covers it), then a fetch-time BTB hit on every later iteration,
;      then a predicted-taken/not-taken mispredict on the exit. The sum is right
;      only if the hits steer to the loop top and EX recovers once at the exit.
;   2. one forward branch taken then not-taken at the same PC — pass 1 allocates
;      it taken; pass 2 is a BTB hit that predicts taken while the branch
;      resolves NOT taken, so EX must correct to the fall-through and invalidate
;      the entry. Each block must be reached exactly once (R6 == 2).
;
; Self-checks into R1 (1 = PASS, 0 = FAIL); run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                ; assume FAIL

    ; ── Test 1: a backward loop the BTB learns to predict ───────────
    ; Sum 1..16 = 136. The loop-closing BNE is backward, so it is a fetch-time
    ; BTB hit on iterations 2..16 and a predicted-taken/not-taken mispredict on
    ; the exit. The sum is correct only if every hit steered to loop1 and EX
    ; recovered to the fall-through exactly once.
    LLI  R3, #0                ; accumulator
    LLI  R4, #1                ; i = 1
loop1:
    ADD  R3, R4                ; sum += i
    ADD  R4, #1                ; i++
    CMP  R4, #17
    BNE  loop1                 ; backward — BTB hit after iteration 1
    CMP  R3, #136              ; 1+2+...+16
    BNE  fail

    ; ── Test 2: one forward branch, taken then not-taken at one PC ──
    ; Pass 1 (R5=0): the BEQ is taken (forward) → allocate it in the BTB.
    ; Pass 2 (R5=1): the SAME BEQ is fetched again → BTB hit predicts taken,
    ; but it resolves NOT taken → EX corrects to the fall-through and the entry
    ; is invalidated. Each block runs once, so R6 ends at 2.
    LLI  R6, #0                ; reached-block marker
    LLI  R5, #0                ; pass 1: the branch will be taken
fwd:
    CMP  R5, #0
    BEQ  fwd_taken             ; forward conditional
    ADD  R6, #1                ; pass 2 fall-through (not-taken): reached
    B    fwd_done
fwd_taken:
    ADD  R6, #1                ; pass 1 taken path: reached
    LLI  R5, #1                ; arm pass 2
    B    fwd                   ; re-run the same forward BEQ, now not-taken
fwd_done:
    CMP  R6, #2
    BNE  fail

    ; All checks passed.
    LLI  R1, #1
    BREAK

fail:
    LLI  R1, #0                ; R1 = 0 → FAIL
    BREAK
