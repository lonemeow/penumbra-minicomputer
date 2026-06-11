; test_cpuid.s — Test CPU identity sysreg device (SYSDEV_CPU = 1)
;
; Tests:
;   1. Read CPU_ISA (device 1, reg 0) → expect 0x00000001 (ISA v1, no features)
;   2. Read CPU_NAME0 (device 1, reg 1) → expect 0x756E6550 ("Penu" LE)
;   3. Read CPU_NAME3 (device 1, reg 4) → expect 0 (null padding)
;   4. Read reserved register (device 1, reg 15) → expect 0
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: CPU_ISA = ISA v1, no features ──────────────
    RDSYS R2, #CPU, #CPU_ISA
    CMPI R2, #1                ; {28'd0, 4'd1} = 1
    BNE  fail

    ; ── Test 2: CPU_NAME0 = "Penu" (0x756E6550) ───────────
    RDSYS R2, #CPU, #CPU_NAME0
    LI    R3, #0x756E6550
    CMP   R2, R3
    BNE   fail

    ; ── Test 3: CPU_NAME3 = 0 (null padding) ──────────────
    RDSYS R2, #CPU, #CPU_NAME3
    CMPI R2, #0
    BNE  fail

    ; ── Test 4: Reserved register reads as zero ────────────
    RDSYS R2, #CPU, #15       ; reg 15, reserved
    CMPI R2, #0
    BNE  fail

    ; ── All passed ──────────────────────────────────────────
    LLI  R1, #1               ; PASS
fail:
    BREAK
