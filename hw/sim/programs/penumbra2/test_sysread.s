; test_sysread.s — gen2 RDSYS read-path test (sysreg sideband through MEM).
;
; RDSYS reads a CPU-internal sysreg device via MEM's sideband: the launch cycle
; drives the device/register selectors, the device's registered response arrives
; the next (data-ready) cycle, and MEM delivers it as the GPR writeback — a
; 2-cycle access with the same single-STALL timing as a D-cache hit.
;
; The core wires the real cpuid (device 1) and machid (device 8) identity
; devices behind the sideband. cpuid carries the CPU name ("Penumbra"); machid
; is unnamed (zeros) in this bring-up harness. The reads check:
;   - cpuid NAME0 → "Penu" and NAME1 → "mbra": the read path works, the
;     register selector discriminates, and the registered response updates per
;     access (no staleness across the two reads);
;   - machid NAME0 (device 8) → 0: the device selector routes by sys_dev — a mux
;     that ignored it would return cpuid's "Penu" here.
; Each RDSYS feeds a dependent CMP, so the scoreboard must also track the
; RDSYS destination through the new 2-cycle sysreg path.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL); run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                ; assume FAIL until every read checks out

    ; ── cpuid NAME0 (dev 1, reg 1) → "Penu" = 0x756E6550 ────────
    RDSYS R2, #1, #1
    LLI  R6, #0x6550
    LUI  R6, #0x756E           ; R6 = 0x756E6550
    CMP  R2, R6
    BNE  fail

    ; ── cpuid NAME1 (dev 1, reg 2) → "mbra" = 0x6172626D ────────
    RDSYS R3, #1, #2
    LLI  R7, #0x626D
    LUI  R7, #0x6172           ; R7 = 0x6172626D
    CMP  R3, R7
    BNE  fail

    ; ── machid NAME0 (dev 8, reg 1) → 0 (unnamed); proves dev-select ──
    RDSYS R4, #8, #1
    LLI  R8, #0
    CMP  R4, R8
    BNE  fail

    LLI  R1, #1               ; all sysreg reads correct → PASS
fail:
    BREAK
