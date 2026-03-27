; Penumbra load/store integration test
;
; Phase 1: Store and load back (basic)
; Phase 2: Offset addressing
; Phase 3: Store overwrites previous value

        ; ── Phase 1: Basic store + load ──────────────────────
        LLI  R1, #42           ; R1 = 42 (value to store)
        LLI  R2, #0x100        ; R2 = 0x100 (base address)
        STW  R1, [R2]          ; mem[0x100] = 42
        LLI  R1, #0            ; R1 = 0 (clear to prove load works)
        LDW  R3, [R2]          ; R3 = mem[0x100] = 42

        ; ── Phase 2: Offset addressing ──────────────────────
        LLI  R4, #99           ; R4 = 99
        STW  R4, [R2 + #4]     ; mem[0x104] = 99
        LDW  R5, [R2 + #4]     ; R5 = mem[0x104] = 99
        LDW  R6, [R2]          ; R6 = mem[0x100] = 42 (still there)

        ; ── Phase 3: Overwrite ──────────────────────────────
        LLI  R7, #0xFF         ; R7 = 255
        STW  R7, [R2]          ; mem[0x100] = 255 (overwrite)
        LDW  R8, [R2]          ; R8 = mem[0x100] = 255
