; test_mem.s — Load/store integration test
;
; Tests:
;   1. Basic store and load back
;   2. Offset addressing
;   3. Store overwrites previous value
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: Basic store + load ──────────────────────────
    LLI  R2, #42              ; value to store
    LLI  R3, #0x100           ; base address
    STW  R2, [R3]             ; mem[0x100] = 42
    LDW  R4, [R3]             ; R4 = mem[0x100]
    CMP  R4, R2
    BNE  fail

    ; ── Test 2: Offset addressing ───────────────────────────
    LLI  R5, #99
    STW  R5, [R3 + #4]        ; mem[0x104] = 99
    LDW  R6, [R3 + #4]        ; R6 = mem[0x104]
    CMP  R6, R5
    BNE  fail
    LDW  R7, [R3]             ; R7 = mem[0x100] = 42 (still there)
    CMP  R7, R2
    BNE  fail

    ; ── Test 3: Overwrite ───────────────────────────────────
    LLI  R8, #0xFF
    STW  R8, [R3]             ; mem[0x100] = 255 (overwrite)
    LDW  R9, [R3]             ; R9 = mem[0x100]
    CMP  R9, R8
    BNE  fail

    ; ── All checks passed ───────────────────────────────────
    LLI  R1, #1               ; PASS
fail:
    BREAK
