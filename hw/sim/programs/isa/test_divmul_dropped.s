; test_divmul_dropped.s — a divide a trap drops leaves nothing in the next.
;
; The load faults on alignment, and the DIVU behind it may already have
; started a multi-cycle divide when the trap drops it. The handler divides
; at once and must get its own quotient, never the dropped divide's: a
; core that starts the handler's divide while the unit still works on the
; dropped one fails here.
;
; The load's base comes from a divide, so on a core whose fetch runs ahead
; of issue the DIVU is fetched, and directly behind the load, by the time
; the load goes.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL).

_start:
    LLI  R1, #0                 ; assume FAIL until the handler checks

    ; ── Install the alignment-fault handler into the vector table ──
    LA   R2, align_handler
    LLI  R3, #0x20              ; VEC_ALIGN (8) << 2
    STW  R2, [R3]

    ; ── The dropped divide's operands: 1000 / 3 = 333 ─────────────
    LLI  R6, #1000
    LLI  R7, #3

    ; ── The load's base, late ────────────────────────────────────
    LLI  R4, #0x200
    LLI  R11, #1
    DIVU R4, R11                ; R4 = 0x200

    LDW  R5, [R4 + #1]          ; EA = 0x201: misaligned, faults
    DIVU R6, R7                 ; dropped by the fault

    ; ── Poison: must be flushed by the fault ──────────────────────
    LLI  R1, #0
    BREAK

align_handler:
    LLI  R8, #100
    LLI  R9, #7
    DIVU R8, R9                 ; 100 / 7 = 14
    CMPI R8, #14
    BNE  fail
    LLI  R1, #1                 ; PASS
fail:
    BREAK
