; bp_basic.s -- control transfers whose predictions are known.
;
; Run with +bpmodel; the window runs from reset to the BREAK.  The whole
; program fits in 32 words, so no two branch sites share an entry of a
; 32-entry buffer or a 64-entry table.  In order, the transfers are:
;
;   C1  BNE loop, backward: taken, taken, not taken
;   BL func from A, then its return; BL func from B, then its return
;   I1  JMP R6 to ind_t, twice, around
;   C2  BNE loop2, backward: taken, not taken
;   JALR R7 to func, then its return
;   C3  BEQ skip, forward: taken
;
; The return stack never holds more than one entry and never pops
; empty.  run_iss_tests.py derives each predictor's redirects.

_start:
    LLI  R4, #3
loop:
    SUB  R4, #1
    BNE  loop                  ; C1
    BL   func                  ; A
    BL   func                  ; B
    LLI  R4, #2
    LA   R6, #ind_t
loop2:
    JMP  R6                    ; I1
ind_t:
    SUB  R4, #1
    BNE  loop2                 ; C2
    LA   R7, #func
    JALR R7
    CMP  R4, #0
    BEQ  skip                  ; C3
    LLI  R1, #99
skip:
    BREAK

func:
    JMP  R13
