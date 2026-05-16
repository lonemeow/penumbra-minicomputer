; test_scratch_sprs.s — RDSPR/WRSPR SCR0..SCR3 functional check
;
; Tests:
;   1. WRSPR each SCR with a distinct 32-bit pattern, then RDSPR each
;      back.  Verifies the write/read path for all four.
;   2. After all four are written, re-read each — verifies that
;      writing one SCR does not disturb the others.
;   3. Overwrite SCR2 with a new pattern; verify SCR2 sees the new
;      value and SCR0/SCR3 are still intact.
;
; Distinct per-SCR patterns catch any cross-wiring in the readback
; mux or write-enable decode.
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0                  ; assume fail

    ; ── Build distinct patterns in R2/R4/R5/R6 ────────────────
    LLI   R2, #0xCDDE            ; SCR0 ← 0xAABB_CDDE
    LUI   R2, #0xAABB
    LLI   R4, #0x3344            ; SCR1 ← 0x1122_3344
    LUI   R4, #0x1122
    LLI   R5, #0x7788            ; SCR2 ← 0x5566_7788
    LUI   R5, #0x5566
    LLI   R6, #0xBEEF            ; SCR3 ← 0xDEAD_BEEF
    LUI   R6, #0xDEAD

    ; ── Test 1: WRSPR each SCR ────────────────────────────────
    WRSPR SCR0, R2
    WRSPR SCR1, R4
    WRSPR SCR2, R5
    WRSPR SCR3, R6

    ; ── Test 2: RDSPR each — roundtrip + independence ────────
    RDSPR R3, SCR0
    CMP   R3, R2
    BNE   fail
    RDSPR R3, SCR1
    CMP   R3, R4
    BNE   fail
    RDSPR R3, SCR2
    CMP   R3, R5
    BNE   fail
    RDSPR R3, SCR3
    CMP   R3, R6
    BNE   fail

    ; ── Test 3: overwrite SCR2; SCR0/SCR3 must be unchanged ──
    LLI   R10, #0x1234           ; new SCR2 value 0xCAFE_1234
    LUI   R10, #0xCAFE
    WRSPR SCR2, R10
    RDSPR R3, SCR2
    CMP   R3, R10
    BNE   fail

    RDSPR R3, SCR0               ; SCR0 still 0xAABB_CDDE
    CMP   R3, R2
    BNE   fail

    RDSPR R3, SCR3               ; SCR3 still 0xDEAD_BEEF
    CMP   R3, R6
    BNE   fail

    ; All tests passed
    LLI  R1, #1
    BREAK

fail:
    BREAK
