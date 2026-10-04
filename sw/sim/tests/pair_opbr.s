; pair_opbr.s -- what the op-and-branch fusion takes, beside the
; compare-and-branch and MOV-and-op fusions.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  In order, each ahead of a
; conditional branch:
;
;   Q1  ADD R3, R1             an ALU op that writes its register: opbr
;   Q2  CMP R1, R2             a compare writes none: cmpbr, not opbr
;   Q3  SHL R3, #1             a shift: neither
;   Q4  ADC R3, R1             an add with carry: neither
;   Q5  MOV R5, R6; ADD R5, #1 movop takes the MOV and the ADD, leaving
;                              the branch; without movop, opbr takes the
;                              ADD and the branch
;   Q7  MOV R7, R6; SUB R3, #1 the MOV is independent: opbr, and the
;                              fused op can join it
;   Q6  SUB R4, #1             a counter and its taken branch: opbr
;
; Only Q6's branch is taken.  run_iss_tests.py derives each stage's
; fusions and pairs.

_start:
    LLI  R1, #1
    DI
    LLI  R2, #2
    DI
    LLI  R4, #2
    DI

    ADD  R3, R1                ; Q1
    BEQ  q1_target
q1_target:
    DI

    CMP  R1, R2                ; Q2
    BEQ  q2_target
q2_target:
    DI

    SHL  R3, #1                ; Q3
    BEQ  q3_target
q3_target:
    DI

    ADC  R3, R1                ; Q4
    BEQ  q4_target
q4_target:
    DI

    MOV  R5, R6                ; Q5
    ADD  R5, #1
    BEQ  q5_target
q5_target:
    DI

    MOV  R7, R6                ; Q7
    SUB  R3, #1
    BEQ  q7_target
q7_target:
    DI

    SUB  R4, #1                ; Q6
    BNE  q6_target
    LLI  R1, #99
q6_target:
    DI
    BREAK
