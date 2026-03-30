; test_align_data.s — Data alignment faults on load/store
;
; Tests (no MMU needed — alignment checked even in bypass mode):
;   1. LDW from addr+1 (word misaligned) → align fault, FAULT_STATUS has ACC_READ
;   2. STW to addr+2 (word misaligned)   → align fault, FAULT_STATUS has ACC_WRITE
;   3. LDH from addr+1 (half misaligned) → align fault, FAULT_STATUS has ACC_READ
;   4. STH to addr+3 (half misaligned)   → align fault, FAULT_STATUS has ACC_WRITE
;   5. LDB from addr+1 (byte, always OK) → no fault
;   6. STB to addr+3 (byte, always OK)   → no fault
;
; VEC_ALIGN = 8, vector table offset 0x20.
; FAULT_STATUS layout: {20'b0, user_mode, access_type[2:0], 4'b0, FAULT_ALIGN(3)}
;   Supervisor read align:  0x00000103
;   Supervisor write align: 0x00000203
;
; Register convention:
;   R1      = pass/fail
;   R7      = test phase (1-4)
;   R10     = expected FAULT_ADDR
;   R11     = expected FAULT_STATUS
;   R8      = handler success tag (set to R7)
;   R9,R12  = handler scratch
;
; Result: R1=1 PASS, R1=0 FAIL

.equ VEC_ALIGN_OFF, 0x20
.equ FSTAT_R_ALIGN, 0x103   ; ACC_READ + FAULT_ALIGN, supervisor
.equ FSTAT_W_ALIGN, 0x203   ; ACC_WRITE + FAULT_ALIGN, supervisor
.equ DATA_BASE,     0x1000  ; word-aligned base for test data

_start:
    LLI  R1, #0

    ; ── Install alignment fault handler ─────────────────────
    LA   R2, #align_handler
    STW  R2, [R0 + #VEC_ALIGN_OFF]

    ; ── Plant test data at DATA_BASE ────────────────────────
    LI   R3, #0xDEADBEEF
    LI   R4, #DATA_BASE
    STW  R3, [R4]             ; mem[0x1000] = 0xDEADBEEF

    ; ── Test 1: LDW from misaligned address (base+1) ────────
    LLI  R7, #1
    LLI  R8, #0
    LI   R10, #0x1001         ; expected FAULT_ADDR
    LLI  R11, #FSTAT_R_ALIGN  ; expected FAULT_STATUS
    LDW  R5, [R4 + #1]       ; should fault
after_test1:
    CMP  R8, #1
    BNE  fail

    ; ── Test 2: STW to misaligned address (base+2) ──────────
    LLI  R7, #2
    LLI  R8, #0
    LI   R10, #0x1002
    LLI  R11, #FSTAT_W_ALIGN
    STW  R5, [R4 + #2]       ; should fault
after_test2:
    CMP  R8, #2
    BNE  fail

    ; ── Test 3: LDH from misaligned address (base+1) ────────
    LLI  R7, #3
    LLI  R8, #0
    LI   R10, #0x1001
    LLI  R11, #FSTAT_R_ALIGN
    LDH  R5, [R4 + #1]       ; should fault
after_test3:
    CMP  R8, #3
    BNE  fail

    ; ── Test 4: STH to misaligned address (base+3) ──────────
    LLI  R7, #4
    LLI  R8, #0
    LI   R10, #0x1003
    LLI  R11, #FSTAT_W_ALIGN
    STH  R5, [R4 + #3]       ; should fault
after_test4:
    CMP  R8, #4
    BNE  fail

    ; ── Test 5: LDB from odd address (no fault expected) ────
    LDB  R5, [R4 + #1]       ; byte: always aligned, no fault

    ; ── Test 6: STB to odd address (no fault expected) ──────
    STB  R5, [R4 + #3]       ; byte: always aligned, no fault

    ; All tests passed
    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Alignment fault handler
;
; Verifies FAULT_ADDR == R10, FAULT_STATUS == R11, sets R8 = R7,
; advances EPC+4 (skip faulting instruction), and ERETs.
; ═══════════════════════════════════════════════════════════════
align_handler:
    RDSYS R9, #MMU, #FAULT_ADDR
    CMP   R9, R10
    BNE   fail
    RDSYS R9, #MMU, #FAULT_STATUS
    CMP   R9, R11
    BNE   fail
    MOV   R8, R7
    RDSPR R9, EPC
    ADD   R9, #4              ; skip faulting instruction (can't fix alignment)
    RDSPR R12, ESR
    ERET  R12, R9
