; test_cpu_perfctr.s — Test CPU performance counters in SYSDEV_CPU
; REQUIRES: perfctr
;
; Counters are free-running, 32-bit, reset on system reset.  This test
; verifies that:
;   1. Both counters are non-zero by the time we read them (program has
;      already executed several instructions before this point).
;   2. cycles >= insns_retired (CPI >= 1 always — fewer cycles than
;      instructions would be impossible without superscalar dispatch).
;   3. Counters monotonically increase: a second read gives a strictly
;      larger value than the first.
;
; ISS note: the ISS exposes insn_count for both regs, so cycles will
; equal insns_retired (IPC = 1) which still satisfies cycles >= insns.
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: insns_retired is non-zero ──────────────────
    RDSYS R2, #CPU, #CPU_INSNS_RETIRED
    CMPI  R2, #0
    BEQ   fail

    ; ── Test 2: cycles is non-zero ─────────────────────────
    RDSYS R3, #CPU, #CPU_CYCLES
    CMPI  R3, #0
    BEQ   fail

    ; ── Test 3: cycles >= insns_retired (CPI >= 1) ─────────
    ; R3 (cycles) was sampled after R2 (insns), so R3 should be
    ; at least R2 — even on the ISS where IPC = 1 exactly.
    CMP   R3, R2
    BLT   fail                ; cycles < insns -> impossible

    ; ── Test 4: counters monotonically increase ────────────
    RDSYS R4, #CPU, #CPU_CYCLES
    CMP   R4, R3              ; second cycles read must exceed first
    BLE   fail
    RDSYS R5, #CPU, #CPU_INSNS_RETIRED
    CMP   R5, R2              ; second insns read must exceed first
    BLE   fail

    ; ── All passed ──────────────────────────────────────────
    LLI  R1, #1               ; PASS
fail:
    BREAK
