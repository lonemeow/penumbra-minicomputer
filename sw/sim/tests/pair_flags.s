; pair_flags.s -- pairs decided by the flags.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  No younger op reads a
; register its older op writes, so only the flags decide.  In order:
;
;   P1  ADD; ADD           both write the flags
;   P2  ADD; MOV           only the older writes them
;   P3  CMP; BEQ           the younger branches on what the older writes
;   P4  ADD; ADC           the younger adds in the carry the older writes,
;                          and writes the flags itself
;   P5  MOV; BEQ           the younger branches on flags of an earlier cycle
;   P6  MOV; ADC           the younger adds in a carry of an earlier cycle
;   P8  ADD; CMP; BEQ      a flag writer, then a compare and its branch
;
; No branch is taken.  run_iss_tests.py derives each stage's pairs.

_start:
    LLI  R1, #1
    DI
    LLI  R2, #2
    DI

    ADD  R3, R1                ; P1
    ADD  R4, R1
    DI

    ADD  R3, R1                ; P2
    MOV  R5, R6
    DI

    CMP  R1, R2                ; P3
    BEQ  p3_target
p3_target:
    DI

    ADD  R3, R1                ; P4
    ADC  R4, R1
    DI

    MOV  R5, R6                ; P5
    BEQ  p5_target
p5_target:
    DI

    MOV  R5, R6                ; P6
    ADC  R4, R1
    DI

    ADD  R3, R1                ; P8
    CMP  R1, R2
    BEQ  p8_target
p8_target:
    DI
    BREAK
