; test_machid.s — Test machine identity sysreg device (SYSDEV_MACH = 8)
;
; Tests:
;   1. Read MACH_FEAT (device 8, reg 0) → expect 0 (no machine features)
;   2. Read MACH_NAME0 (device 8, reg 1) → expect 0x756D6953 ("Simu" LE)
;   3. Read CPU_FREQ (device 8, reg 5) → expect 25_000_000 (simulated 25 MHz)
;   4. Read reserved register (device 8, reg 15) → expect 0
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: MACH_FEAT = 0 (no features set) ───────────
    RDSYS R2, #MACH, #MACH_FEAT
    CMPI R2, #0
    BNE  fail

    ; ── Test 2: MACH_NAME0 = "Simu" (0x756D6953) ──────────
    RDSYS R2, #MACH, #MACH_NAME0
    LI    R3, #0x756D6953
    CMP   R2, R3
    BNE   fail

    ; ── Test 3: CPU_FREQ = 25 MHz (simulated) ─────────────
    RDSYS R2, #MACH, #CPU_FREQ
    LI    R3, #25000000
    CMP   R2, R3
    BNE   fail

    ; ── Test 4: Reserved register reads as zero ────────────
    RDSYS R2, #MACH, #15      ; reg 15, reserved
    CMPI R2, #0
    BNE  fail

    ; ── All passed ──────────────────────────────────────────
    LLI  R1, #1               ; PASS
fail:
    BREAK
