; penumbra2_smoke.s — gen2 first-light: straight-line ALU + RAW stall + BREAK.
;
; No branches (deferred to the branch milestone), so the testbench checks the
; committed register *values* rather than having the program self-check into
; R1. This exercises real instruction fetch from the i-mem through IF1/IF2,
; decode, the ALU, the scoreboard RAW stall under real fetch, and the
; halt-on-BREAK path.
;
; Expected committed state at BREAK:  R1 = 8,  R2 = 3,  R3 = 24.

_start:
    LLI  R1, #5
    LLI  R2, #3
    LLI  R3, #16
    ADD  R1, R2        ; R1 = 5 + 3  = 8     (producer)
    ADD  R3, R1        ; R3 = 16 + 8 = 24    (RAW on R1 — scoreboard stalls until R1 commits)
    BREAK
