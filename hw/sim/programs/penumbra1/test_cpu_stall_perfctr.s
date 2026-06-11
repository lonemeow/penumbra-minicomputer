; test_cpu_stall_perfctr.s — CPU stall-attribution counters (SYSDEV_CPU 7-10)
;
; Verifies the four mutually-exclusive stall counters route each stall to
; the correct bucket.  The core property is ISOLATION: a burst of one op
; class must not move the other classes' counters.
;
; Dual-target note: these run on both the ISS and RTL.  The ISS is
; instruction-accurate, not cycle-accurate, so every stall counter reads
; 0 there.  Isolation checks (wrong-bucket delta == 0) hold on both.  The
; one positive check ("a MUL burst raises FUNIT") only holds where stalls
; exist, so it is gated on detecting cycle-accuracy (CPI > 1).
;
; Result: R1=1 PASS, R1=0 FAIL

.equ TEST_ADDR, 0x0800        ; low RAM, reachable in MMU-bypass mode

_start:
    LLI  R1, #0               ; assume fail

    ; ── Isolation 1: a MUL burst must not move LOAD or STORE ──
    RDSYS R2, #CPU, #STALL_LOAD
    RDSYS R3, #CPU, #STALL_STORE
    LLI  R7, #6
    LLI  R8, #7
    MUL  R7, R8
    MUL  R7, R8
    MUL  R7, R8
    MUL  R7, R8
    RDSYS R4, #CPU, #STALL_LOAD
    RDSYS R5, #CPU, #STALL_STORE
    CMP  R4, R2
    BNE  fail                 ; LOAD moved during a MUL burst
    CMP  R5, R3
    BNE  fail                 ; STORE moved during a MUL burst

    ; ── Isolation 2: a STORE burst must not move FUNIT or LOAD ──
    LLI  R9, #TEST_ADDR
    RDSYS R2, #CPU, #STALL_FUNIT
    RDSYS R3, #CPU, #STALL_LOAD
    STW  R7, [R9]
    STW  R7, [R9]
    STW  R7, [R9]
    STW  R7, [R9]
    RDSYS R4, #CPU, #STALL_FUNIT
    RDSYS R5, #CPU, #STALL_LOAD
    CMP  R4, R2
    BNE  fail                 ; FUNIT moved during a STORE burst
    CMP  R5, R3
    BNE  fail                 ; LOAD moved during a STORE burst

    ; ── Isolation 3: a LOAD burst must not move FUNIT or STORE ──
    RDSYS R2, #CPU, #STALL_FUNIT
    RDSYS R3, #CPU, #STALL_STORE
    LDW  R10, [R9]
    LDW  R10, [R9]
    LDW  R10, [R9]
    LDW  R10, [R9]
    RDSYS R4, #CPU, #STALL_FUNIT
    RDSYS R5, #CPU, #STALL_STORE
    CMP  R4, R2
    BNE  fail                 ; FUNIT moved during a LOAD burst
    CMP  R5, R3
    BNE  fail                 ; STORE moved during a LOAD burst

    ; ── Detect cycle-accuracy ─────────────────────────────────
    ; The ISS returns insn_count for both CYCLES and INSNS_RETIRED, so on
    ; the ISS cycles_delta == insns_delta exactly (CPI = 1).  Cycle-
    ; accurate RTL always has CPI > 1 (separate fetch + execute cycles).
    ; R11 = 1 on RTL, 0 on the ISS.
    RDSYS R5, #CPU, #CPU_CYCLES
    RDSYS R6, #CPU, #CPU_INSNS_RETIRED
    RDSYS R7, #CPU, #CPU_CYCLES
    RDSYS R8, #CPU, #CPU_INSNS_RETIRED
    SUB  R7, R5               ; R7 = cycles delta
    SUB  R8, R6               ; R8 = insns delta
    LLI  R11, #0              ; assume ISS
    CMP  R7, R8
    BLE  env_done             ; cycles <= insns -> ISS (no stalls modeled)
    LLI  R11, #1              ; cycles  > insns -> cycle-accurate RTL
env_done:

    ; ── Positive: on cycle-accurate HW, a MUL burst MUST raise FUNIT ──
    RDSYS R2, #CPU, #STALL_FUNIT       ; FUNIT before
    LLI  R7, #6
    LLI  R8, #7
    MUL  R7, R8
    MUL  R7, R8
    MUL  R7, R8
    MUL  R7, R8
    RDSYS R4, #CPU, #STALL_FUNIT       ; FUNIT after
    SUB  R4, R2                        ; R4 = FUNIT delta over the burst

    CMP  R11, R0
    BEQ  pos_done

    CMP  R4, #0
    BEQ  fail

pos_done:
    LLI  R1, #1               ; PASS
fail:
    BREAK
