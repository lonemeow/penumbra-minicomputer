; test_smoke.s — gen2 smoke test: straight-line ALU + RAW stall + BREAK.
;
; The original first-light program: real instruction fetch from the i-mem
; through IF1/IF2, decode, the ALU, the scoreboard RAW stall under real fetch,
; and the halt-on-BREAK path. The ALU/RAW sequence is unchanged from first
; light; a self-check tail (added once branches worked) folds the expected
; values into R1 so the generic runner's R1 == 1 convention applies. gen2 has
; no register write-through, so a missed scoreboard stall lets the RAW-
; dependent ADD read a stale R1 and the final compares catch the wrong value.

_start:
    LLI  R1, #5
    LLI  R2, #3
    LLI  R3, #16
    ADD  R1, R2        ; R1 = 5 + 3  = 8     (producer)
    ADD  R3, R1        ; R3 = 16 + 8 = 24    (RAW on R1 — scoreboard stalls until R1 commits)

    ; ── Self-check: R1 == 8, R2 == 3, R3 == 24 ──────────────────
    CMP  R1, #8
    BNE  fail
    CMP  R2, #3
    BNE  fail
    CMP  R3, #24
    BNE  fail
    LLI  R1, #1        ; PASS
    BREAK
fail:
    LLI  R1, #0
    BREAK
