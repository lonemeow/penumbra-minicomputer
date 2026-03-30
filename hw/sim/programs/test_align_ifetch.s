; test_align_ifetch.s — Instruction fetch alignment fault
;
; Tests:
;   1. JMP to address with bit 0 set (0x2001) → alignment fault
;   2. JMP to address with bit 1 set (0x2002) → alignment fault
;   3. JMP to word-aligned address (0x2000) → no fault, executes normally
;
; VEC_ALIGN = 8, vector table offset 0x20.
; Handler checks EPC matches the expected misaligned address.
; No MMU needed — alignment is checked before the TLB lookup.
;
; Register convention:
;   R1      = pass/fail result
;   R7      = test phase (1 or 2)
;   R10     = expected misaligned EPC
;   R8      = handler success tag (set to R7 by handler)
;   R9,R11  = handler scratch
;
; Result: R1=1 PASS, R1=0 FAIL

.equ VEC_ALIGN_OFF, 0x20   ; vector[8] offset in table

_start:
    LLI  R1, #0

    ; ── Install alignment fault handler ─────────────────────
    LA   R2, #align_handler
    STW  R2, [R0 + #VEC_ALIGN_OFF]

    ; ── Plant target code at 0x2000 (word-aligned) ──────────
    ; For test 3: code that sets R1=1 and returns via R13
    LA   R2, #target_code
    LLI  R3, #0x2000
    LA   R4, #target_code_end

copy:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy

    ; ── Test 1: JMP to odd address (bit 0 set) ──────────────
    LLI  R7, #1
    LLI  R8, #0
    LI   R10, #0x2001         ; expected EPC
    LI   R6, #0x2001
    JMP  R6                   ; should alignment-fault

    ; Handler ERETs here
after_test1:
    CMP  R8, #1               ; handler sets R8 = R7 = 1 on success
    BNE  fail

    ; ── Test 2: JMP to halfword-aligned address (bit 1 set) ─
    LLI  R7, #2
    LLI  R8, #0
    LI   R10, #0x2002         ; expected EPC
    LI   R6, #0x2002
    JMP  R6                   ; should alignment-fault

after_test2:
    CMP  R8, #2               ; handler sets R8 = R7 = 2 on success
    BNE  fail

    ; ── Test 3: JMP to word-aligned address (no fault) ──────
    ; BL sets R13 = pass (return addr), trampoline JMPs to 0x2000,
    ; planted code does LLI R1, #1 then RET → lands at pass.
    BL   trampoline

pass:
    ; R1 already set to 1 by target code
fail:
    BREAK

trampoline:
    LA   R12, #0x2000
    JMP  R12

; ═══════════════════════════════════════════════════════════════
; Alignment fault handler
;
; Verifies EPC == R10 (expected misaligned address), sets
; R8 = R7 (test phase tag), and ERETs to after_testN.
; ═══════════════════════════════════════════════════════════════
align_handler:
    RDSPR R9, EPC
    CMP   R9, R10
    BNE   fail

    ; Tag success for this test phase
    MOV   R8, R7

    ; Select return address based on test phase
    CMP   R7, #1
    BNE   not_test1
    LA    R9, #after_test1
    B     do_eret
not_test1:
    LA    R9, #after_test2
do_eret:
    WRSPR EPC, R9
    ERET

; ═══════════════════════════════════════════════════════════════
; Target code — copied to RAM at 0x2000 for test 3
; ═══════════════════════════════════════════════════════════════
target_code:
    LLI  R1, #1
    RET
target_code_end:
