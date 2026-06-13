; test_fault.s — gen2 exception-entry test: a data alignment fault
; vectors to its handler.
;
; Exercises the whole synchronous-fault path end to end: a misaligned load is
; tagged VEC_ALIGN in MEM, takes the fault at WB (flushing the pipeline and
; pulsing save-state), and the vector-fetch FSM reads the handler address from
; the vector table and redirects PC to it. The handler proves it ran by setting
; the PASS flag. The poison between the faulting load and the handler must be
; flushed: if the redirect or flush failed, the poison BREAK retires with R1=0.
;
; The vector table lives in RAM (physical low), written here at run time — only
; reachable by the vector fetch because fetch and data share one unified memory.
; Self-checks into R1 (1 = PASS, 0 = FAIL); run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                 ; assume FAIL until the handler runs

    ; ── Install the alignment-fault handler into the vector table ──
    LA   R2, align_handler      ; R2 = &align_handler (in ROM)
    LLI  R3, #0x20              ; VEC_ALIGN (8) << 2 = 0x20  (table slot, RAM)
    STW  R2, [R3]              ; vector_table[VEC_ALIGN] = handler address

    ; ── Trigger a misaligned word load → VEC_ALIGN ──────────────
    LLI  R4, #0x200            ; aligned base in RAM
    LDW  R5, [R4 + #1]         ; EA = 0x201, word-misaligned → alignment fault

    ; ── Poison: must be flushed by the fault redirect ───────────
    LLI  R1, #0                ; if reached, clears the PASS flag (FAIL)
    BREAK                      ; if reached, retires before the handler (FAIL)

align_handler:
    LLI  R1, #1                ; PASS — control reached the handler via the vector
    BREAK
