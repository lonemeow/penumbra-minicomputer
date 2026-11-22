; pair_fused.s -- what a fused op may pair with (fpair:).
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  Every register starts at
; zero, so the compares find their operands equal and no branch is
; taken.  No probe has a dependency.  In order:
;
;   P  LLI R1; LUI R1; ADD             a fused constant, then a simple op
;   Q  ADD; CMP R5, R6; BNE            a simple op, then a fused branch
;   R  LLI R1; LUI R1; CMP; BNE        two fused ops
;   S  CMP R5, R6; BNE; LDW            a fused branch, then a load
;   T  MOV R1, R2; ADD R1, R3; LDW     a fused ALU op, then a load
;   U  ADD; MOV                        two simple ops, neither fused
;   V  LDW; MOV R1, R2; ADD R1, R3     a load, then a fused ALU op
;
; run_iss_tests.py derives each stage's fusions and pairs.

_start:
    LLI  R10, #0x1000
    DI

    LLI  R1, #1                ; P
    LUI  R1, #0
    ADD  R2, R3
    DI

    ADD  R2, R3                ; Q
    CMP  R5, R6
    BNE  q_skip
q_skip:
    DI

    LLI  R1, #1                ; R
    LUI  R1, #0
    CMP  R5, R6
    BNE  r_skip
r_skip:
    DI

    CMP  R5, R6                ; S
    BNE  s_skip
    LDW  R7, [R10]
s_skip:
    DI

    MOV  R1, R2                ; T
    ADD  R1, R3
    LDW  R7, [R10]
    DI

    ADD  R2, R3                ; U
    MOV  R8, R11
    DI

    LDW  R7, [R10]             ; V
    MOV  R1, R2
    ADD  R1, R3
    DI
    BREAK
