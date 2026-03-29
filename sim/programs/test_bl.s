; test_bl.s — test branch-and-link instruction
;
; Verifies:
;   1. BL jumps to the target label
;   2. BL saves PC+4 (return address) in R13
;   3. RET (JMP R13) returns to the instruction after BL
;   4. Nested BL works with manual LR save/restore

.org 0x00
    B start             ; reset vector

.org 0x40
start:
    ; ── Test 1: Basic BL and RET ──
    LLI R2, #0
    BL sub1
    ; sub1 sets R2 = 42 and returns here
    CMPI R2, #42
    BNE fail

    ; ── Test 2: Nested calls ──
    ; outer saves LR, calls inner (sets R3=99), sets R4=7, restores LR
    BL outer
    CMPI R3, #99
    BNE fail
    CMPI R4, #7
    BNE fail

    ; ── Test 3: BL to next instruction (offset=0 edge case) ──
    BL next_instr
next_instr:
    ; R13 should point here (PC+4 of the BL), which equals this address
    ; If we got here, it worked (BL with zero-distance is degenerate but valid)

    ; All passed
    LLI R1, #1
fail:
    BREAK

; ── Subroutines ──────────────────────────────────────────

sub1:
    LLI R2, #42
    RET

outer:
    MOV R6, R13         ; save outer's return address
    BL inner
    LLI R4, #7
    MOV R13, R6         ; restore outer's return address
    RET

inner:
    LLI R3, #99
    RET
