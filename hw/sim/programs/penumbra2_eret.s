; penumbra2_eret.s — gen2 exception round-trip: fault → handler → ERET → resume.
;
; A misaligned load takes VEC_ALIGN and vectors to a handler. The handler fixes
; the cause — it corrects the base register so the *re-executed* load is aligned
; — then ERETs. ERET restores SR from ESR and redirects PC to EPC (the faulting
; load), which now succeeds, and execution falls through to the PASS flag. If
; ERET failed to return to EPC (or the base fix were lost), the load would fault
; again forever and the run would time out instead of reaching BREAK.
;
; Fixing the operand instead of advancing EPC keeps the test free of WRSPR/RDSPR
; (the SPR access path is not wired into the pipeline yet). Self-checks into R1.

_start:
    LLI  R1, #0                 ; assume FAIL

    ; install the alignment handler in the vector table (RAM)
    LA   R2, align_handler
    LLI  R3, #0x20             ; VEC_ALIGN (8) << 2
    STW  R2, [R3]

    LLI  R4, #0x200            ; base; first try EA = 0x201 (misaligned)
retry:
    LDW  R5, [R4 + #1]         ; faults first time; after the handler fixes R4,
                               ;   EA = 0x1FF + 1 = 0x200 (aligned) on re-execute
    LLI  R1, #1                ; PASS — the load completed after the ERET return
    BREAK

align_handler:
    LLI  R4, #0x1FF            ; fix the cause: make the retried EA word-aligned
    ERET                       ; SR <- ESR; PC <- EPC (re-execute the load)
