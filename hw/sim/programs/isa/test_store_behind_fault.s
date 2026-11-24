; test_store_behind_fault.s — a store right behind a faulting load never
; reaches memory.
;
; Exceptions are precise: nothing after the faulting instruction takes
; effect. The load faults on alignment, and the store behind it would
; overwrite a canary word the handler then checks. A pipelined core that
; lets the store reach memory in the cycles between deciding the fault and
; taking it fails here.
;
; The faulting load's base comes from two divides, which take many cycles
; and no bus access, so on a core whose fetch runs ahead of issue the store
; is fetched, and directly behind the load, by the time the load goes.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL).

_start:
    LLI  R1, #0                 ; assume FAIL until the handler checks

    ; ── Install the alignment-fault handler into the vector table ──
    LA   R2, align_handler
    LLI  R3, #0x20              ; VEC_ALIGN (8) << 2
    STW  R2, [R3]

    ; ── The canary, and the value the store would put over it ─────
    LLI  R6, #0x300
    LLI  R7, #0x55
    STW  R7, [R6]               ; mem[0x300] = canary
    LLI  R8, #0xAA

    ; ── The load's base, late ────────────────────────────────────
    LLI  R4, #0x200
    LLI  R11, #1
    DIVU R4, R11                ; R4 = 0x200
    DIVU R4, R11                ; R4 = 0x200

    LDW  R5, [R4 + #1]          ; EA = 0x201: misaligned, faults
    STW  R8, [R6]               ; must not reach memory

    ; ── Poison: must be flushed by the fault ──────────────────────
    LLI  R1, #0
    BREAK

align_handler:
    LDW  R9, [R6]
    CMP  R9, R7                 ; the canary is still there
    BNE  fail
    LLI  R1, #1                 ; PASS
fail:
    BREAK
