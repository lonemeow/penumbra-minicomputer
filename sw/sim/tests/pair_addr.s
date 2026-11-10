; pair_addr.s -- what the address fusions take: a scaled index into an
; add (shadd), and an add into a load's or store's base (addmem).
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  In order:
;
;   A  SHL R5, #2; ADD R5, R6      a word index into a sum: shadd
;   B  SHL R5, #4; ADD R5, R6      a scale past three: no shadd
;   C  SHL R5, #1; ADD R5, R5      the ADD reads the shifted R5 twice: no shadd
;   D  SHL R5, #2; ADD R6, R5      the sum in another register: no shadd
;   E  ADD R9, R10; LDW R9, [R9]   a load that overwrites its base: addmem
;   F  ADD R9, R10; LDW R8, [R9]   a load into another register would
;                                  leave two writes: no addmem
;   G  ADD R9, R10; STW R5, [R9+4] a store through the sum: addmem
;   H  ADD R9, #4; LDW R9, [R9]    an immediate, not a register: no addmem
;   I  ADD R9, R10; BEQ            opbr's pair, which addmem alone leaves
;   J  SHL; ADD; LDW               the chain: shadd takes the SHL and the
;                                  ADD when it is on, addmem the ADD and
;                                  the load when it alone is
;
; run_iss_tests.py derives each stage's fusions and pairs.

_start:
    LLI  R10, #0x1000
    DI
    LLI  R6, #0x100
    DI

    SHL  R5, #2                ; A
    ADD  R5, R6
    DI

    SHL  R5, #4                ; B
    ADD  R5, R6
    DI

    SHL  R5, #1                ; C
    ADD  R5, R5
    DI

    SHL  R5, #2                ; D
    ADD  R6, R5
    DI

    ADD  R9, R10               ; E
    LDW  R9, [R9]
    DI

    ADD  R9, R10               ; F
    LDW  R8, [R9]
    DI

    ADD  R9, R10               ; G
    STW  R5, [R9 + #4]
    DI

    ADD  R9, #4                ; H
    LDW  R9, [R9]
    DI

    ADD  R9, R10               ; I
    BEQ  i_target
i_target:
    DI

    LLI  R5, #0x400
    DI
    LLI  R6, #0
    DI

    SHL  R5, #2                ; J
    ADD  R5, R6
    LDW  R5, [R5]
    DI
    BREAK
