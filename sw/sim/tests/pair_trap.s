; pair_trap.s -- an instruction that traps issues alone, and so does the
; one ahead of it.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A
; misaligned load traps to VEC_ALIGN, whose handler steps EPC past it
; and returns.  In order: LLI and LUI (the handler's address), STW (the
; vector), LLI, DI, MOV, the LDW that traps, the handler's RDSPR, ADD,
; WRSPR and ERET, MOV, BREAK.  run_iss_tests.py derives each stage's
; pairs.

_start:
    LA   R2, #align_fault
    STW  R2, [R0 + #0x20]      ; vector 8, read physically
    LLI  R10, #0x1000
    DI
    MOV  R5, R6
    LDW  R7, [R10 + #1]
    MOV  R5, R6
    BREAK

align_fault:
    RDSPR R2, EPC
    ADD  R2, #4
    WRSPR EPC, R2
    ERET
