; bp_ras.s -- a call chain six deep, for the return stack's overflow.
;
; Run with +bpmodel; the window runs from reset to the BREAK.  _start
; calls f1, each fN calls fN+1 up to f6, and each returns through R13
; after restoring it.  A 4-deep ring keeps the four innermost return
; addresses: the four innermost returns predict right, and the two
; outermost find stale slots or, counting its entries, none -- a miss
; either way.  A stack that refused pushes once full would keep the four
; outermost and mispredict every return.  run_iss_tests.py derives the
; counts.

_start:
    BL   f1
    BREAK

f1:
    MOV  R5, R13
    BL   f2
    MOV  R13, R5
    JMP  R13

f2:
    MOV  R6, R13
    BL   f3
    MOV  R13, R6
    JMP  R13

f3:
    MOV  R7, R13
    BL   f4
    MOV  R13, R7
    JMP  R13

f4:
    MOV  R8, R13
    BL   f5
    MOV  R13, R8
    JMP  R13

f5:
    MOV  R9, R13
    BL   f6
    MOV  R13, R9
    JMP  R13

f6:
    JMP  R13
