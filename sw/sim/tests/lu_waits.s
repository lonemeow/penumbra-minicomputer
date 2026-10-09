; lu_waits.s -- what waits for a load's value, and for what an ALU op
; behind a load writes.
;
; Run with +loadusemodel; the window runs from reset to the BREAK.  After
; the setup, which stores a pointer and a jump's target, each probe
; starts with a load:
;
;   G  ADD R12, R10; LDW [R12]          an ALU result as a base, no load
;                                       ahead of it
;   A  LDW R1; ADD R2, R1               the load's value as an ALU operand
;   B  LDW R3; LDW [R3]                 as a base
;   C  LDW R5; STW R5                   as store data
;   D  LDW R6; CMP R6, #0; BNE          through a compare to a branch,
;                                       taken at its first execution
;   E  LDW R7; ADD R8, R7; ADD R9, R8   through an ALU op to another
;   F  LDW R1; ADD R1, #0; LDW [R1]     through an ALU op to a base
;   M  LDW R3; MUL R4, R3               as a multiplier's operand
;   T  LDW R5; JMP R5                   as a jump's target
;   H  CMP R10, #0; BEQ                 flags with no load behind them, to
;                                       a branch not taken
;
; run_iss_tests.py derives each pipeline's waits.

_start:
    LLI  R10, #0x1000
    LA   R11, #t_target
    STW  R11, [R10 + #4]
    STW  R10, [R10]

    ADD  R12, R10              ; G
    LDW  R13, [R12]

    LDW  R1, [R10]             ; A
    ADD  R2, R1

    LDW  R3, [R10]             ; B
    LDW  R4, [R3]

    LDW  R5, [R10]             ; C
    STW  R5, [R10]

    LDW  R6, [R10]             ; D
    CMP  R6, #0
    BNE  d_taken
    DI                         ; skipped
d_taken:

    LDW  R7, [R10]             ; E
    ADD  R8, R7
    ADD  R9, R8

    LDW  R1, [R10]             ; F
    ADD  R1, #0
    LDW  R2, [R1]

    LDW  R3, [R10]             ; M
    MUL  R4, R3

    LDW  R5, [R10 + #4]        ; T
    JMP  R5
    DI                         ; skipped
t_target:

    CMP  R10, #0               ; H
    BEQ  h_next
h_next:
    BREAK
