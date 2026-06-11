; test_spr_sr.s — RDSPR SR / WRSPR SR: read and write the status register
; REQUIRES: wrspr
;
; Tests:
;   1. RDSPR SR reads current SR (expect S=1, I=0 at boot)
;   2. WRSPR SR can set SR.I (interrupt enable)
;   3. DI + RDSPR SR shows I=0
;   4. Save/restore pattern: RDSPR SR, DI, WRSPR SR restores I bit
;
; Hardware SR layout (from penumbra_pkg.sv):
;   Bit 31: S (supervisor)
;   Bit 30: I (interrupt enable)
;   Bits 0-3: N, Z, C, V (condition flags)
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: RDSPR SR at boot ─────────────────────────────
    ; At boot: supervisor mode (S=1), interrupts disabled (I=0)
    RDSPR R2, SR
    ; S bit is bit 31 — check via shift
    MOV   R3, R2
    SHR   R3, #31              ; isolate S bit
    CMP   R3, #1
    BNE   fail                 ; S bit should be set

    ; I bit is bit 30
    MOV   R3, R2
    SHR   R3, #30
    AND   R3, #1               ; isolate I bit
    CMP   R3, #0
    BNE   fail                 ; I bit should be clear at boot

    ; ── Test 2: WRSPR SR to enable interrupts ────────────────
    RDSPR R2, SR
    LLI   R10, #0
    LUI   R10, #0x4000         ; bit 30 = I
    OR    R2, R10              ; set I=1
    WRSPR SR, R2
    RDSPR R3, SR
    MOV   R4, R3
    SHR   R4, #30
    AND   R4, #1
    CMP   R4, #1
    BNE   fail                 ; I should now be set

    ; ── Test 3: DI clears I, visible via RDSPR SR ───────────
    DI
    RDSPR R3, SR
    MOV   R4, R3
    SHR   R4, #30
    AND   R4, #1
    CMP   R4, #0
    BNE   fail                 ; I should be clear after DI

    ; ── Test 4: Save/restore pattern ─────────────────────────
    ; Enable interrupts first via WRSPR SR
    RDSPR R2, SR
    OR    R2, R10              ; R10 still has bit 30
    WRSPR SR, R2

    ; Now do a save/restore cycle (the atomic operation pattern)
    RDSPR R5, SR               ; save SR (I=1)
    DI                         ; disable interrupts
    RDSPR R6, SR
    MOV   R7, R6
    SHR   R7, #30
    AND   R7, #1
    CMP   R7, #0
    BNE   fail                 ; I should be 0 after DI

    WRSPR SR, R5               ; restore saved SR (I=1)
    RDSPR R6, SR
    MOV   R7, R6
    SHR   R7, #30
    AND   R7, #1
    CMP   R7, #1
    BNE   fail                 ; I should be restored to 1

    ; ── Test 5: S bit preserved across save/restore ──────────
    MOV   R7, R6
    SHR   R7, #31
    CMP   R7, #1
    BNE   fail                 ; S should still be set

    ; All tests passed
    LLI  R1, #1
    BREAK

fail:
    BREAK
