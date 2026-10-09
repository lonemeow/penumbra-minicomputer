; ops_muldiv.s -- +opstats counts by window, and the steps MUL and DIV
; operands need.
;
; Run with +opstats +model_start=0xFFFF0800 +model_stop=0xFFFF0900.  A
; multiply and a divide run before the window, in supervisor and in user
; mode, and again after it; none of them may count.  Inside it every
; operand is one LLI or one MOV, so the classes count by eye:
;
;   M1  MULU 0x13 by 0x1234             5 bits by 13
;   M2  MULU 0x10000 by 3               17 by 2
;   M3  MUL  -2 by 0x100                signed: 2 by 9
;   M4  MULU 0xFFFFFFFE by 0            the same word unsigned: 32 by none
;   D1  DIVU 0x800 by 0x10              12 bits by 5
;   D2  DIVU 0x1000 by 0x10             13 by 5
;   D3  DIVU 0xFFFFFFFF by 0x10000      32 by 17
;   D4  DIVU 0x100000 by 0x10           21 by 5
;   D5  DIVU 0xFFFFFFFF by 1            32 by 1
;   D6  DIVU 5 by 100                   3 by 7: the dividend is the shorter
;   D7  DIVU 12 by 9                    4 by 4
;   D8  DIV  -1000 by -10               signed: 10 by 4
;   D9  DIVU 0 by 7                     none by 3
;   DZ  DIVU 5 by 0                     traps to VEC_ARITH, whose handler
;                                       steps EPC past it and returns
;
; run_iss_tests.py derives the counts.

.equ ROM_PTE,   0xFFFF00E9     ; V|R|X|U|G, uncached
.equ USER_SR,   0x4000         ; upper half: S=0

_start:
    LA   R3, #arith_fault
    STW  R3, [R0 + #0x28]      ; vector 10, read physically
    ; Pinned slot 0: the ROM page, user-executable
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    LLI  R1, #3                ; before the window, in supervisor
    LLI  R2, #5
    MULU R1, R2
    LLI  R1, #100
    LLI  R2, #7
    DIVU R1, R2

    LLI  R2, #0
    LUI  R2, #USER_SR
    WRSPR ESR, R2
    LA   R2, #user_entry
    WRSPR EPC, R2
    ERET

arith_fault:
    RDSPR R3, EPC
    ADD  R3, #4
    WRSPR EPC, R3
    ERET

user_entry:
    LI   R7,  #0xFFFFFFFE      ; -2
    LI   R8,  #0xFFFFFFF6      ; -10
    LI   R9,  #0xFFFFFC18      ; -1000
    LI   R10, #0xFFFFFFFF
    LI   R11, #0x10000
    LI   R12, #0x100000
    LLI  R1, #3                ; before the window, in user mode
    LLI  R2, #5
    MULU R1, R2
    LLI  R1, #100
    LLI  R2, #7
    DIVU R1, R2
    B    window_start

.org 0xFFFF0800
window_start:
    LLI  R1, #0x13             ; M1
    LLI  R2, #0x1234
    MULU R1, R2
    MOV  R1, R11               ; M2
    LLI  R2, #3
    MULU R1, R2
    MOV  R1, R7                ; M3
    LLI  R2, #0x100
    MUL  R1, R2
    MOV  R1, R7                ; M4
    LLI  R2, #0
    MULU R1, R2

    LLI  R1, #0x800            ; D1
    LLI  R2, #0x10
    DIVU R1, R2
    LLI  R1, #0x1000           ; D2
    LLI  R2, #0x10
    DIVU R1, R2
    MOV  R1, R10               ; D3
    MOV  R2, R11
    DIVU R1, R2
    MOV  R1, R12               ; D4
    LLI  R2, #0x10
    DIVU R1, R2
    MOV  R1, R10               ; D5
    LLI  R2, #1
    DIVU R1, R2
    LLI  R1, #5                ; D6
    LLI  R2, #100
    DIVU R1, R2
    LLI  R1, #12               ; D7
    LLI  R2, #9
    DIVU R1, R2
    MOV  R1, R9                ; D8
    MOV  R2, R8
    DIV  R1, R2
    LLI  R1, #0                ; D9
    LLI  R2, #7
    DIVU R1, R2
    LLI  R1, #5                ; DZ
    LLI  R2, #0
    DIVU R1, R2
    B    window_stop

.org 0xFFFF0900
window_stop:
    LLI  R1, #3                ; after the window
    LLI  R2, #5
    MULU R1, R2
    LLI  R1, #100
    LLI  R2, #7
    DIVU R1, R2
    BREAK
