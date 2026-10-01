; pair_deps.s -- pairs decided by a register the younger op reads and
; the older writes.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  Each probe's younger op
; reads the register its older op writes, and what forward covers it
; depends on how the value is produced and how it is used:
;
;   B  LLI; LUI            a constant, as an ALU operand (LA is LLI + LUI)
;   A  MOV; ADD #imm       a copy, as an ALU operand
;   C  LUI; STW            an EX result, as store data
;   D  MOV; LDW            a copy, as a base
;   E  ADD #imm; LDW       a register plus an immediate, as a base
;   F  LUI; LDW            any other EX result, as a base
;   G  SHL #imm; ADD       an EX result, as an ALU operand
;   H  LDW; STW            a load's data, as store data
;   I  MOV; JMP            a copy, as a jump target
;
; No younger op reads the flags.  run_iss_tests.py derives each stage's
; pairs.

_start:
    LLI  R10, #0x1000
    DI

    LA   R7, #i_target         ; B
    DI

    MOV  R5, R6                ; A
    ADD  R5, #1
    DI

    LUI  R5, #1                ; C
    STW  R5, [R10]
    DI

    MOV  R9, R10               ; D
    LDW  R8, [R9]
    DI

    ADD  R9, #4                ; E
    LDW  R8, [R9]
    DI

    LUI  R9, #0                ; F
    LDW  R8, [R9]
    DI

    SHL  R5, #1                ; G
    ADD  R6, R5
    DI

    LDW  R5, [R10]             ; H
    STW  R5, [R10 + #4]
    DI

    MOV  R6, R7                ; I
    JMP  R6
    LLI  R1, #99
i_target:
    DI
    BREAK
