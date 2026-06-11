; test_nested_trap.s — Kernel takes an inner trap while servicing
; REQUIRES: wrspr
; an outer trap (SYSCALL → ALIGN fault from inside the handler)
;
; Sequence under test:
;   1. User code (supervisor in this test, but the path is the same
;      shape) issues SYSCALL.
;   2. SYSCALL handler explicitly saves outer ESR/EPC to memory —
;      this models what NetBSD does in its trapframe-save path,
;      where the C handler may legitimately fault on copyin().
;   3. Handler then deliberately performs a misaligned load.  This
;      triggers VEC_ALIGN.  The alignment-fault entry overwrites
;      ESR and EPC with the SYSCALL handler's state.
;   4. ALIGN handler advances EPC past the faulting load and ERETs.
;   5. SYSCALL handler resumes immediately after the faulting load,
;      restores the outer ESR/EPC from its saved copies, and ERETs.
;   6. User code resumes after the SYSCALL.  Test passes when R1=1.
;
; What this validates:
;   - Inner trap entry correctly captures the *current* (= SYSCALL
;     handler's) PC + SR into ESR/EPC — not a stale outer value.
;   - Inner ERET resumes at the correct address (the instruction
;     after the faulting load, not somewhere else).
;   - Saving + restoring the outer ESR/EPC across an inner trap
;     round-trips cleanly (no SPR write-then-read corruption).
;   - Outer ERET still works after ESR/EPC were overwritten and
;     manually restored.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ SAVE_EPC_ADDR, 0x100
.equ SAVE_ESR_ADDR, 0x104

_start:
    LLI   R1, #0                  ; assume fail

    ; ── Install vectors ────────────────────────────────────
    LA    R2, #syscall_handler
    STW   R2, [R0 + #0x14]        ; vector[5] = VEC_SYSCALL

    LA    R2, #align_handler
    STW   R2, [R0 + #0x20]        ; vector[8] = VEC_ALIGN

    ; ── Trigger outer trap ─────────────────────────────────
caller_pc:
    SYSCALL

    ; ── Post-SYSCALL: success criteria ─────────────────────
    ; SYSCALL handler ERETs here.  R4 was set to 1 by the
    ; handler if the nested-fault round-trip worked.
    CMP   R4, #1
    BNE   fail

    ; All good
    LLI   R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; SYSCALL handler (outer trap)
; ═══════════════════════════════════════════════════════════════
syscall_handler:
    ; ── Save outer trap context ─────────────────────────────
    ; The SYSCALL trap captured ESR/EPC for the user-mode frame.
    ; The deliberate misaligned load below will overwrite both,
    ; so we mirror them to memory first.
    RDSPR R7, EPC
    STW   R7, [R0 + #SAVE_EPC_ADDR]
    RDSPR R8, ESR
    STW   R8, [R0 + #SAVE_ESR_ADDR]

    ; ── Provoke the inner trap: misaligned word load ───────
    LLI   R5, #1                  ; address with bit-0 set → unaligned
faulting_load:
    LDW   R6, [R5]                ; VEC_ALIGN fires here

    ; ── ALIGN handler ERETs to here.  Mark success. ────────
    LLI   R4, #1                  ; R4 = "nested round-trip succeeded"

    ; ── Restore outer context and ERET to user ─────────────
    LDW   R7, [R0 + #SAVE_EPC_ADDR]
    ADD   R7, #4                  ; advance past SYSCALL
    WRSPR EPC, R7
    LDW   R8, [R0 + #SAVE_ESR_ADDR]
    WRSPR ESR, R8
    ERET

; ═══════════════════════════════════════════════════════════════
; ALIGN handler (inner trap)
; ═══════════════════════════════════════════════════════════════
align_handler:
    ; Verify the inner trap captured the *handler's* PC, not the
    ; user's caller_pc.  EPC should equal &faulting_load.
    LA    R9, #faulting_load
    RDSPR R10, EPC
    CMP   R10, R9
    BNE   bad_inner_epc           ; FAIL via the global fail BREAK

    ; Skip past the faulting load and return to the SYSCALL handler.
    ADD   R10, #4
    WRSPR EPC, R10
    ERET

bad_inner_epc:
    ; Inner trap captured the wrong EPC.  Force an obvious failure
    ; by ERETing somewhere that hits the test's fail BREAK.
    LA    R10, #fail
    WRSPR EPC, R10
    ERET
