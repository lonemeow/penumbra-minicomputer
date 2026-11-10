; pair_wb.s -- the address fusions that write their address back
; (addwb, incmem), and the two write ports a pair shares.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  In order:
;
;   A  ADD R9, R10; LDW R8, [R9]   a load into another register: addwb
;   B  ADD R9, #4; LDW R8, [R9]    a pre-incremented load: incmem
;   C  SUB R9, #4; STW R5, [R9]    a pre-decremented store: incmem
;   D  MOV R7, R6; ADD; LDW R8     the copy and the fused load would make
;                                  three writes: no pair
;   E  ADD; LDW R8; CMP R6, R7     the compare writes no register: the
;                                  fused load and it pair
;   F  ADD R9, #4; LDW R9, [R9]    a pre-incremented load into its base:
;                                  incmem, one write
;   G  ADD R9, R10; LDW R9, [R9]   addmem's, which addwb leaves
;
; run_iss_tests.py derives each stage's fusions and pairs.

_start:
    LLI  R10, #0x1000
    DI
    LLI  R9, #0x1000
    DI

    ADD  R9, R10               ; A
    LDW  R8, [R9]
    DI

    ADD  R9, #4                ; B
    LDW  R8, [R9]
    DI

    SUB  R9, #4                ; C
    STW  R5, [R9]
    DI

    MOV  R7, R6                ; D
    ADD  R9, R10
    LDW  R8, [R9]
    DI

    ADD  R9, R10               ; E
    LDW  R8, [R9]
    CMP  R6, R7
    DI

    ADD  R9, #4                ; F
    LDW  R9, [R9]
    DI

    LLI  R9, #0x1000
    DI

    ADD  R9, R10               ; G
    LDW  R9, [R9]
    DI
    BREAK
