; pair_limits.s -- the registers a fused op may read (reads:), and the
; classes that are always the younger op of a pair (last:).
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  Every register starts at
; zero, so the compares find their operands equal and no branch is
; taken.  In order:
;
;   A  ADD R9, R10; STW R5, [R9]   a store of a third register: reads three
;   B  ADD R9, R10; STW R10, [R9]  the data is the add's own operand: two
;   C  ADD R9, R10; STW R9, [R9]   storing the sum: two
;   D  ADD R9, R10; LDW R9, [R9]   a load: two
;   E  CMP R5, R6; BNE; MOV        a fused branch, then an op that may join it
;   F  BNE; MOV                    a branch alone, likewise
;   G  LDW; MOV                    a load, likewise
;   H  MOV; BNE                    a branch behind an op
;   I  MOV; LDW                    a load behind an op
;
; run_iss_tests.py derives each stage's fusions and pairs.

_start:
    LLI  R10, #0x1000
    DI

    ADD  R9, R10               ; A
    STW  R5, [R9]
    DI

    ADD  R9, R10               ; B
    STW  R10, [R9]
    DI

    ADD  R9, R10               ; C
    STW  R9, [R9]
    DI

    ADD  R9, R10               ; D
    LDW  R9, [R9]
    DI

    CMP  R5, R6                ; E
    BNE  e_skip
    MOV  R7, R8
e_skip:
    DI

    BNE  f_skip                ; F
    MOV  R7, R8
f_skip:
    DI

    LDW  R7, [R10]             ; G
    MOV  R8, R11
    DI

    MOV  R8, R11               ; H
    BNE  h_skip
h_skip:
    DI

    MOV  R8, R11               ; I
    LDW  R7, [R10]
    DI
    BREAK
