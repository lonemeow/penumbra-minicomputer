; pair_fetch.s -- a stream placed for the fetch stage: lines of MOVs, a
; taken branch out of the second line, and a target at an odd word.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  The
; program starts on a 16-byte line.  Executed, with each instruction's
; line and its word in the line:
;
;   line 0  words 0-3   MOV MOV MOV MOV
;   line 1  words 0-2   MOV MOV B          the B is taken
;   line 2  words 1-3   MOV MOV MOV        the B's target is word 1
;   line 3  words 0-1   MOV BREAK
;
; Every MOV writes a register of its own, and all but one copy R2, so
; two adjacent MOVs pair.  The exception is the last MOV of line 2, which
; copies what the MOV ahead of it writes: those two do not pair, so which
; two instructions of that line arrive together shows in the pairs.
; run_iss_tests.py derives each stage's cycles and fetches.

_start:
    MOV  R1, R2
    MOV  R3, R2
    MOV  R4, R2
    MOV  R5, R2

    MOV  R6, R2
    MOV  R7, R2
    B    target
    MOV  R8, R2                ; skipped

    MOV  R8, R2                ; skipped
target:
    MOV  R9, R2
    MOV  R10, R2
    MOV  R11, R10              ; reads the MOV ahead of it

    MOV  R1, R2
    BREAK
