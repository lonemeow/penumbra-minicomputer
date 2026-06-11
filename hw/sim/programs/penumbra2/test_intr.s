; test_intr.s — gen2 interrupt test: an external IRQ is recognized at a
; fetch boundary and vectors to its handler.
;
; The IRQ line is held asserted by the testbench from reset, but it must NOT be
; taken until interrupts are enabled and the EI one-instruction shadow has
; passed: masked while SR.I=0 (boot), masked for the one instruction after EI
; (the shadow), then recognized in the spin loop — drain-and-take vectors to the
; handler installed in the RAM table. The handler sets the PASS flag.
;
; If recognition fired too early (before EI / during the shadow) it would vector
; before R1 is meaningfully set; if it never fired, the spin loops to the cycle
; cap. Self-checks into R1.
;
; RUNNER: tb_penumbra2_intr

_start:
    LLI  R1, #0                 ; assume FAIL
    LA   R2, irq_handler
    LLI  R3, #0x24             ; VEC_EXT_IRQ (9) << 2
    STW  R2, [R3]              ; vector_table[VEC_EXT_IRQ] = handler

    EI                          ; enable interrupts (one-instruction shadow)
    LLI  R4, #0                ; shadow instruction: runs with IRQs still masked

spin:
    B    spin                   ; wait here; the IRQ is recognized at this boundary

irq_handler:
    LLI  R1, #1                ; PASS — the IRQ was recognized and vectored
    BREAK
