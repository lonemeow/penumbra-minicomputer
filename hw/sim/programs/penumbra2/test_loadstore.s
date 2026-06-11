; test_loadstore.s — gen2 MEM-stage data-path test (loads + stores).
;
; Exercises the now-wired MEM data path end-to-end through the real fetch loop:
; word/halfword/byte store-then-load round-trips against the BRAM data memory,
; sub-word extract (zero-extend) on load, and byte_en lane isolation (a byte
; store must replace exactly one lane, leaving its neighbours intact). Each
; store-then-load pair is naturally ordered by the single MEM stage — the store
; fully completes its 2-cycle access (write on the advancing cycle) before the
; load enters MEM — so no register dependency is needed to order them.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL), the repo-wide hw-test convention.
; Data lives at 0x100+ (word 0x40 of the flat data memory), clear of the code.

_start:
    LLI  R1, #0               ; assume FAIL until the final PASS
    LLI  R3, #0x100           ; base data address (word-aligned)

    ; ── Word round-trip ─────────────────────────────────────────
    LLI  R2, #0x1234
    LUI  R2, #0x8765          ; R2 = 0x87651234
    STW  R2, [R3]
    LDW  R4, [R3]
    CMP  R4, R2               ; loaded word must match what was stored
    BNE  fail

    ; ── Byte store lands in exactly one lane (no neighbour clobber) ──
    LLI  R5, #0xFF
    STB  R5, [R3 + #1]        ; replace byte 1 of 0x87651234 → 0x8765FF34
    LDW  R4, [R3]
    LLI  R6, #0xFF34
    LUI  R6, #0x8765          ; R6 = 0x8765FF34 (expected after the byte poke)
    CMP  R4, R6
    BNE  fail

    ; ── Byte load zero-extends the selected lane ────────────────
    LDB  R7, [R3 + #1]        ; reads 0xFF, zero-extended
    LLI  R8, #0xFF
    CMP  R7, R8
    BNE  fail

    ; ── Halfword round-trip ─────────────────────────────────────
    LLI  R9, #0xBEEF
    STH  R9, [R3 + #8]        ; write low half at 0x108
    LDH  R10, [R3 + #8]       ; read it back, zero-extended
    CMP  R10, R9
    BNE  fail

    LLI  R1, #1               ; all checks passed → PASS
fail:
    BREAK
