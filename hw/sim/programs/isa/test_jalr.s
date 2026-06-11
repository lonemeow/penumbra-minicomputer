; test_jalr.s — test jump-and-link-register instruction
;
; Verifies:
;   1. JALR jumps to the address in Rs
;   2. JALR saves PC+4 (return address) in R13
;   3. RET (JMP R13) returns to the instruction after JALR
;   4. JALR works with different target registers

_start:
    ; ── Test 1: Basic JALR and RET ──
    LLI R2, #0
    LA R5, sub1
    JALR R5
    ; sub1 sets R2 = 42 and returns here
    CMP R2, #42
    BNE fail

    ; ── Test 2: JALR with nested calls ──
    ; outer_ind loads inner's address, calls via JALR
    LA R5, outer_ind
    JALR R5
    CMP R3, #99
    BNE fail
    CMP R4, #7
    BNE fail

    ; ── Test 3: JALR via a different register (R8) ──
    LLI R2, #0
    LA R8, sub1
    JALR R8
    CMP R2, #42
    BNE fail

    ; All passed
    LLI R1, #1
fail:
    BREAK

; ── Subroutines ──────────────────────────────────────────

sub1:
    LLI R2, #42
    RET

outer_ind:
    MOV R6, R13         ; save return address
    LA R7, inner_ind
    JALR R7             ; indirect call to inner_ind
    LLI R4, #7
    MOV R13, R6         ; restore return address
    RET

inner_ind:
    LLI R3, #99
    RET
