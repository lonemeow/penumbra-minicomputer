; Penumbra CPU integration test program
;
; Phase 1: Basic ALU (no interrupts)
; Phase 2: IRQ blocked when SR.I=0
; Phase 3: IRQ after EI with ei_shadow

        LLI  R1, #5         ; R1 = 5
        LLI  R5, #0xFF      ; R5 = 0xFF (also IRQ handler at vector 1 = 0x04)
        LLI  R2, #3         ; R2 = 3
        ADD  R1, R2          ; R1 = 5 + 3 = 8
        LLI  R3, #0          ; R3 = 0
        EI                   ; Enable interrupts (ei_shadow blocks next instr)
        LLI  R4, #0x42       ; R4 = 0x42 (executes under ei_shadow)
        LLI  R6, #0xDD       ; R6 = 0xDD (first IRQ-eligible dispatch)
        LLI  R7, #0xEE       ; R7 = 0xEE
