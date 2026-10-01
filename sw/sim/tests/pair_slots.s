; pair_slots.s -- pairs decided by slots, units, memory accesses and
; taken transfers.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe, so no probe pairs with its
; neighbours.  No older op of a probe writes the flags or a register its
; younger op reads.  In order:
;
;   A  MOV; LDW            a simple op, then a load
;   B  LDW; MOV            a load, then a simple op
;   C  LDW; LDW            two loads
;   D  STW; LDW            a store, then a load
;   E  STW; STW            two stores
;   F  MOV; B, then the instruction B lands on
;   G  B alone: a taken transfer with nothing ahead of it
;   H  BEQ not taken; MOV
;   I  BEQ not taken; BEQ not taken
;
; run_iss_tests.py derives each stage's pairs.

_start:
    LLI  R10, #0x1000
    DI
    LLI  R1, #1
    DI
    CMP  R1, #0                ; Z = 0: no BEQ below is taken
    DI

    MOV  R5, R6                ; A
    LDW  R7, [R10]
    DI

    LDW  R7, [R10]             ; B
    MOV  R5, R6
    DI

    LDW  R7, [R10]             ; C
    LDW  R8, [R10 + #4]
    DI

    STW  R6, [R10]             ; D
    LDW  R8, [R10 + #4]
    DI

    STW  R6, [R10]             ; E
    STW  R6, [R10 + #4]
    DI

    MOV  R5, R6                ; F
    B    f_target
    LLI  R1, #99
f_target:
    MOV  R5, R6
    DI

    B    g_target              ; G
    LLI  R1, #99
g_target:
    MOV  R5, R6
    DI

    BEQ  h_target              ; H
    MOV  R5, R6
h_target:
    DI

    BEQ  i_target              ; I
    BEQ  i_target
i_target:
    DI
    BREAK
