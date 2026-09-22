; test_cmpc.s — CMPC (compare with carry): SBC with the F bit set
;
; Verifies:
;   1. CMPC takes its borrow from C and leaves Rd and Rs unchanged
;   2. CMP + CMPC compares 64-bit values: C, N and V match one
;      subtraction of the whole values; Z reflects the high word only
;   3. Signed compares where the high word overflows (V set)
;   4. Signed compares decided by the borrow from the low word
;
; Convention: R1=1 PASS, R1=0 FAIL
; Pattern: "test; Bcc ok; BREAK" — on failure the PC of the
;          BREAK immediately identifies which check failed.

_start:
    LLI  R1, #0                ; assume fail

    ; ================================================================
    ; CMPC Rd, Rs — flags = Rd - Rs - ~C, no register write
    ; ================================================================

    ; C set (no borrow): 7 - 7 - 0 = 0
    LLI  R2, #7
    LLI  R3, #7
    CMP  R0, R0                ; C=1
    CMPC R2, R3
    BEQ  cmpc_ok1
    BREAK
cmpc_ok1:
    BCS  cmpc_ok2
    BREAK
cmpc_ok2:

    ; C clear (borrow): 7 - 7 - 1 = -1
    ADD  R0, R0                ; C=0
    CMPC R2, R3
    BMI  cmpc_ok3
    BREAK
cmpc_ok3:
    BCC  cmpc_ok4
    BREAK
cmpc_ok4:
    CMP  R2, #7                ; Rd unchanged
    BEQ  cmpc_ok5
    BREAK
cmpc_ok5:
    CMP  R3, #7                ; Rs unchanged
    BEQ  cmpc_ok6
    BREAK
cmpc_ok6:

    ; ================================================================
    ; Unsigned 64-bit: CMP on the low words, CMPC on the high words
    ; a = 0x00000001_00000000, b = 0x00000000_FFFFFFFF
    ; ================================================================
    LLI  R2, #0                ; a lo
    LLI  R3, #1                ; a hi
    LLIS R4, #-1               ; b lo = 0xFFFFFFFF
    LLI  R5, #0                ; b hi

    ; a - b: the low word borrows; the high word is 1 - 0 - 1 = 0
    CMP  R2, R4
    CMPC R3, R5
    BHS  u64_ok1               ; a >= b
    BREAK
u64_ok1:
    BEQ  u64_ok2               ; Z is the high word's, though a != b
    BREAK
u64_ok2:
    CMP  R3, #1                ; Rd unchanged
    BEQ  u64_ok3
    BREAK
u64_ok3:

    ; b - a: the low word does not borrow; the high word is 0 - 1
    CMP  R4, R2
    CMPC R5, R3
    BLO  u64_ok4               ; b < a
    BREAK
u64_ok4:

    ; ================================================================
    ; Signed 64-bit, high word overflows
    ; a = 0x80000000_00000000 (minimum), b = 0x7FFFFFFF_FFFFFFFF (maximum)
    ; ================================================================
    LLI  R2, #0                ; a lo
    LLI  R3, #0
    LUI  R3, #0x8000           ; a hi = 0x80000000
    LLIS R4, #-1               ; b lo = 0xFFFFFFFF
    LLI  R5, #0xFFFF
    LUI  R5, #0x7FFF           ; b hi = 0x7FFFFFFF

    ; a - b = 1 with signed overflow: N=0, V=1, C=1 (a >= b unsigned)
    CMP  R2, R4
    CMPC R3, R5
    BLT  s64v_ok1              ; a < b signed
    BREAK
s64v_ok1:
    BVS  s64v_ok2
    BREAK
s64v_ok2:
    BHS  s64v_ok3              ; a >= b unsigned
    BREAK
s64v_ok3:

    ; b - a = -1 with signed overflow: N=1, V=1
    CMP  R4, R2
    CMPC R5, R3
    BGE  s64v_ok4              ; b >= a signed
    BREAK
s64v_ok4:
    BLO  s64v_ok5              ; b < a unsigned
    BREAK
s64v_ok5:

    ; ================================================================
    ; Signed 64-bit, decided by the low word's borrow
    ; a = 0xFFFFFFFF_00000000, b = 0xFFFFFFFF_00000001 (high words equal)
    ; ================================================================
    LLI  R2, #0                ; a lo
    LLIS R3, #-1               ; a hi
    LLI  R4, #1                ; b lo
    LLIS R5, #-1               ; b hi

    ; a - b = -1: the high word is -1 - -1 - 1 = -1
    CMP  R2, R4
    CMPC R3, R5
    BLT  s64b_ok1              ; a < b signed
    BREAK
s64b_ok1:
    BLO  s64b_ok2              ; a < b unsigned
    BREAK
s64b_ok2:

    ; b - a = 1: the high word is -1 - -1 - 0 = 0
    CMP  R4, R2
    CMPC R5, R3
    BGE  s64b_ok3              ; b >= a signed
    BREAK
s64b_ok3:
    BHS  s64b_ok4              ; b >= a unsigned
    BREAK
s64b_ok4:

    ; a - a: equal values compare >= both ways
    CMP  R2, R2
    CMPC R3, R3
    BGE  s64b_ok5
    BREAK
s64b_ok5:
    BHS  s64b_ok6
    BREAK
s64b_ok6:

    ; All passed
    LLI  R1, #1
    BREAK
