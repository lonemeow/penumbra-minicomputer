; pair_irq.s -- an interrupt entered between two instructions: the one
; ahead of it issues alone, and fetch is redirected behind it.
;
; Run with +pairmodel and +irq_at_pc=0xFFFF0014, the address of the MOV
; at `resumed`; the window runs from reset to the BREAK.  The interrupt
; is taken as that MOV is about to execute, and its handler returns
; there with interrupts off.  In order, with each instruction's word
; from the start of the program:
;
;   words 0-1    LLI, LUI      the handler's address
;   word  2      STW           the vector
;   word  3      EI
;   word  4      MOV           the interrupt is entered behind it
;   words 8-9    LLI, LUI      the handler: a status with interrupts off
;   word  10     WRSPR
;   word  11     ERET
;   words 5-6    MOV, MOV
;   word  7      BREAK
;
; run_iss_tests.py derives each stage's pairs and fetches.

_start:
    LA   R2, #irq_handler
    STW  R2, [R0 + #0x24]      ; vector 9, read physically
    EI
    MOV  R5, R6
resumed:
    MOV  R7, R6
    MOV  R8, R6
    BREAK

irq_handler:
    LI   R2, #0x80000000       ; supervisor, interrupts off
    WRSPR ESR, R2
    ERET
