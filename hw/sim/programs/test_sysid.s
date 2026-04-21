; test_sysid.s — Test system identification registers
;
; Tests:
;   1. Read CPU_ISA (device 1, reg 0) → expect 0x00000001 (ISA v1, no features)
;   2. Read MACH_FEAT (device 1, reg 1) → expect 0 (no machine features)
;   3. Read CPU_NAME0 (device 1, reg 2) → expect 0x756E6550 ("Penu" LE)
;   4. Read CPU_NAME3 (device 1, reg 5) → expect 0 (null padding)
;   5. Read MACH_NAME0 (device 1, reg 6) → expect 0x756D6953 ("Simu" LE)
;   6. Read CPU_FREQ (device 1, reg 10) → expect 25_000_000 (simulated 25 MHz)
;   7. Read reserved register (device 1, reg 15) → expect 0
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: CPU_ISA = ISA v1, no features ──────────────
    RDSYS R2, #SYS, #CPU_ISA
    CMPI R2, #1                ; {28'd0, 4'd1} = 1
    BNE  fail

    ; ── Test 2: MACH_FEAT = 0 (no features set) ───────────
    RDSYS R2, #SYS, #MACH_FEAT
    CMPI R2, #0
    BNE  fail

    ; ── Test 3: CPU_NAME0 = "Penu" (0x756E6550) ───────────
    RDSYS R2, #SYS, #CPU_NAME0
    LI    R3, #0x756E6550
    CMP   R2, R3
    BNE   fail

    ; ── Test 4: CPU_NAME3 = 0 (null padding) ──────────────
    RDSYS R2, #SYS, #CPU_NAME3
    CMPI R2, #0
    BNE  fail

    ; ── Test 5: MACH_NAME0 = "Simu" (0x756D6953) ──────────
    RDSYS R2, #SYS, #MACH_NAME0
    LI    R3, #0x756D6953
    CMP   R2, R3
    BNE   fail

    ; ── Test 6: CPU_FREQ = 25 MHz (simulated) ──────────────
    RDSYS R2, #SYS, #CPU_FREQ
    LI    R3, #25000000
    CMP   R2, R3
    BNE   fail

    ; ── Test 7: Reserved register reads as zero ────────────
    RDSYS R2, #SYS, #15       ; reg 15, reserved
    CMPI R2, #0
    BNE  fail

    ; ── All passed ──────────────────────────────────────────
    LLI  R1, #1               ; PASS
fail:
    BREAK
