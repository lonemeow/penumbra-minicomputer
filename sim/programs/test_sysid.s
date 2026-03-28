; test_sysid.s — Test system identification register
;
; Tests:
;   1. Read MACHINE_ID (device 1, reg 0) → expect 1 (Penumbra/1)
;   2. Read unimplemented register (device 1, reg 1) → expect 0
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: Read MACHINE_ID ─────────────────────────────
    RDSYS R2, #SYS, #MACHINE_ID  ; R2 = MACHINE_ID
    CMPI R2, #1                   ; should be 1 (Penumbra/1)
    BNE  fail

    ; ── Test 2: Unimplemented register reads as zero ────────
    RDSYS R3, #SYS, #1           ; R3 = device 1, reg 1 (unused)
    CMPI R3, #0
    BNE  fail

    ; ── All passed ──────────────────────────────────────────
    LLI  R1, #1               ; PASS
fail:
halt:
    B    halt
