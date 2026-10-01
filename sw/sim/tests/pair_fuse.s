; pair_fuse.s -- instructions the LLI+LUI and MOV+op fusions combine,
; and near misses they leave alone.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  No older op of a pair writes
; the flags.  In order:
;
;   A  LLI R8; LUI R8; LDW [R8]    a fused constant, then a load through it
;   B  LLI R5; LUI R6              different registers: no fusion, no dependency
;   C  LLIS R5; LUI R5             LLIS sign-extends: no fusion
;   D  MOV R5, R6; ADD R5, #1      the op updates the copy in place: fuses
;   E  MOV R5, R6; ADD R6, R5      the op only reads the copy: no fusion
;   F  MOV R5, R6; CMP R5, #1      a compare writes no register: no fusion
;   I  LLI R6; LUI R6; JMP R6      a fused constant, then a jump through it
;
; run_iss_tests.py derives each stage's fusions and pairs.

_start:
    LLI  R1, #1
    DI

    LI   R8, #0x1000           ; A
    LDW  R7, [R8]
    DI

    LLI  R5, #1                ; B
    LUI  R6, #2
    DI

    LLIS R5, #-1               ; C
    LUI  R5, #2
    DI

    MOV  R5, R6                ; D
    ADD  R5, #1
    DI

    MOV  R5, R6                ; E
    ADD  R6, R5
    DI

    MOV  R5, R6                ; F
    CMP  R5, #1
    DI

    LA   R6, #i_target         ; I
    JMP  R6
    LLI  R1, #99
i_target:
    DI
    BREAK
