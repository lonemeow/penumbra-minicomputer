; test_busctl.s — Verify bus controller sysreg (device 4)
; REQUIRES: bus
;
; Tests:
;   1. BUSCTL reads 0 after reset
;   2. Write CFG_EN, read back
;   3. Clear CFG_EN, verify cleared
;   4. Write RST, verify sticky (reads back 1)
;   5. Clear RST explicitly
;   6. Write RST | CFG_EN, clear RST only, verify CFG_EN persists
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: BUSCTL reads 0 after reset ──────────────────
    RDSYS R2, #4, #0          ; read BUSCTL (dev 4, reg 0)
    LLI  R3, #0
    CMP  R2, R3
    BNE  fail

    ; ── Test 2: Write CFG_EN, read back ─────────────────────
    LLI  R4, #2               ; CFG_EN = bit 1
    WRSYS R4, #4, #0          ; write BUSCTL
    RDSYS R2, #4, #0          ; read back
    CMP  R2, R4               ; should be 0x2
    BNE  fail

    ; ── Test 3: Clear CFG_EN ────────────────────────────────
    LLI  R4, #0
    WRSYS R4, #4, #0
    RDSYS R2, #4, #0
    CMP  R2, R4               ; should be 0
    BNE  fail

    ; ── Test 4: Write RST, verify it's sticky ─────────────
    LLI  R4, #1               ; RST = bit 0
    WRSYS R4, #4, #0          ; assert RST
    RDSYS R2, #4, #0          ; read back
    CMP  R2, R4               ; should be 0x1 (RST is sticky)
    BNE  fail

    ; ── Test 5: Clear RST explicitly ────────────────────────
    LLI  R4, #0
    WRSYS R4, #4, #0          ; deassert RST
    RDSYS R2, #4, #0
    CMP  R2, R4               ; should be 0
    BNE  fail

    ; ── Test 6: RST | CFG_EN, then clear RST only ──────────
    LLI  R4, #3               ; RST | CFG_EN
    WRSYS R4, #4, #0
    RDSYS R2, #4, #0
    CMP  R2, R4               ; should be 0x3 (both bits sticky)
    BNE  fail
    LLI  R4, #2               ; CFG_EN only
    WRSYS R4, #4, #0          ; clear RST, keep CFG_EN
    RDSYS R2, #4, #0
    CMP  R2, R4               ; should be 0x2
    BNE  fail

    ; Clean up: disable everything
    LLI  R4, #0
    WRSYS R4, #4, #0

    ; ── PASS ────────────────────────────────────────────────
    LLI  R1, #1
fail:
    BREAK
