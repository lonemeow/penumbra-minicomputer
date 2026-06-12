; test_ifetch_align.s — I-side alignment fault via a misaligned JMP target
;
; A register-sourced JMP delivers a misaligned PC; the fetch raises
; VEC_ALIGN at IF2 and the handler verifies the architectural fault
; registers: FAULT_ADDR is the faulting PC itself, FAULT_STATUS carries
; {user=0, ACC_EXEC, FAULT_ALIGN} = 0x00000403. Works with the MMU off —
; alignment is checked before translation, and the fault registers latch
; for every address-carrying fault, not just TLB ones.
;
; (The isa/ suite's test_align_ifetch covers gen1 via EPC, which needs
; RDSPR — gen2's SPR read path is a later milestone, so this gen2 test
; verifies through FAULT_ADDR/FAULT_STATUS instead.)
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0                 ; assume fail

    ; ── Install the alignment handler: vector[8] at 0x20 ─────
    LA   R2, #align_handler
    STW  R2, [R0 + #0x20]

    ; ── Jump to a misaligned address → fetch faults ──────────
    LLI  R3, #0x2001
    JMP  R3

    ; Fall-through = the fault did not redirect; R1 stays 0.
fail:
    BREAK

align_handler:
    ; FAULT_ADDR = the misaligned PC
    RDSYS R4, #MMU, #FAULT_ADDR
    LLI  R5, #0x2001
    CMP  R4, R5
    BNE  fail

    ; FAULT_STATUS = {user=0, ACC_EXEC, FAULT_ALIGN}
    RDSYS R6, #MMU, #FAULT_STATUS
    LI   R7, #0x403
    CMP  R6, R7
    BNE  fail

    LLI  R1, #1                 ; PASS
    BREAK
