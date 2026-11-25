; test_divmul_dropped_first.s — a divide right after a dropped multiply
; gets its own results.
;
; The load faults on alignment, and the MULU behind it may already have
; started when the trap drops it. The handler's first instruction is a
; divide: a core that still counts the dropped multiply as the operation
; in hand gives the divide the multiply's product and high half, however
; long ago the multiply finished.
;
; The multiply is short, so the unit is idle again before the handler's
; divide arrives on every core: nothing here depends on how quickly the
; handler is fetched.
;
; The load's base comes from a divide, so on a core whose fetch runs ahead
; of issue the MULU is fetched, and directly behind the load, by the time
; the load goes.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL).

_start:
    LLI  R1, #0                 ; assume FAIL until the handler checks

    ; ── Install the alignment-fault handler into the vector table ──
    LA   R2, align_handler
    LLI  R3, #0x20              ; VEC_ALIGN (8) << 2
    STW  R2, [R3]

    ; ── The dropped multiply's operands: 1000 * 3 = 3000, high 0 ──
    LLI  R6, #1000
    LLI  R7, #3

    ; ── The handler's divide's operands: 100 / 7 = 14, rem 2 ─────
    LLI  R8, #100
    LLI  R9, #7

    ; ── The load's base, late ────────────────────────────────────
    LLI  R4, #0x200
    LLI  R11, #1
    DIVU R4, R11                ; R4 = 0x200

    LDW  R5, [R4 + #1]          ; EA = 0x201: misaligned, faults
    MULU R6, R7                 ; dropped by the fault

    ; ── Poison: must be flushed by the fault ──────────────────────
    LLI  R1, #0
    BREAK

align_handler:
    DIVU R8, R9, R10            ; first instruction: quotient R8, remainder R10
    CMPI R8, #14
    BNE  fail
    CMPI R10, #2
    BNE  fail
    LLI  R1, #1                 ; PASS
fail:
    BREAK
