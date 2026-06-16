; test_cpu_stall_perfctr.s — gen2 CPU stall-attribution counters (SYSDEV_CPU 7-10)
;
; Exercises the head-of-line stall breakdown in penumbra2_perfctr: a burst of
; one op class must raise its own STALL_* bucket (positive) and must leave the
; other buckets untouched (isolation). Each bucket is snapshotted via RDSYS
; before and after a 4-op burst:
;   - MUL burst (2-operand, single-dest)   -> STALL_FUNIT up,  LOAD/STORE flat
;   - STW burst                            -> STALL_STORE up,  FUNIT/LOAD flat
;   - LDW burst                            -> STALL_LOAD up,   FUNIT/STORE flat
;
; Positive checks are unconditional: tb_penumbra2_prog is cycle-accurate, every
; divmul holds EX for its iteration, and every load/store pays at least the MEM
; launch-cycle stall — so each burst always moves its own bucket. The RDSYS
; reads between snapshots touch no STALL_* bucket, so the deltas are clean.
; (Cross-bucket overlap — a store stalling in MEM while a younger divmul
; iterates in EX — is resolved to the downstream stall by the perfctr's
; priority and guarded by its mutual-exclusion assertion.)
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ TEST_ADDR, 0x0800        ; low RAM, reachable in MMU-bypass mode

_start:
    LLI  R1, #0               ; assume FAIL until every check passes
    LLI  R9, #TEST_ADDR       ; load/store target

    ; ── MUL burst: raises FUNIT; must not move LOAD or STORE ──
    RDSYS R2, #CPU, #STALL_FUNIT      ; before
    RDSYS R3, #CPU, #STALL_LOAD
    RDSYS R4, #CPU, #STALL_STORE
    LLI  R10, #6
    LLI  R11, #7
    MUL  R10, R11
    MUL  R10, R11
    MUL  R10, R11
    MUL  R10, R11
    RDSYS R5, #CPU, #STALL_FUNIT      ; after
    RDSYS R6, #CPU, #STALL_LOAD
    RDSYS R7, #CPU, #STALL_STORE
    CMP  R6, R3
    BNE  fail                         ; LOAD moved during a MUL burst
    CMP  R7, R4
    BNE  fail                         ; STORE moved during a MUL burst
    CMP  R5, R2
    BEQ  fail                         ; FUNIT did not move on a MUL burst

    ; ── STW burst: raises STORE; must not move FUNIT or LOAD ──
    RDSYS R2, #CPU, #STALL_FUNIT
    RDSYS R3, #CPU, #STALL_LOAD
    RDSYS R4, #CPU, #STALL_STORE
    STW  R10, [R9]
    STW  R10, [R9]
    STW  R10, [R9]
    STW  R10, [R9]
    RDSYS R5, #CPU, #STALL_FUNIT
    RDSYS R6, #CPU, #STALL_LOAD
    RDSYS R7, #CPU, #STALL_STORE
    CMP  R5, R2
    BNE  fail                         ; FUNIT moved during a STORE burst
    CMP  R6, R3
    BNE  fail                         ; LOAD moved during a STORE burst
    CMP  R7, R4
    BEQ  fail                         ; STORE did not move on a STORE burst

    ; ── LDW burst: raises LOAD; must not move FUNIT or STORE ──
    RDSYS R2, #CPU, #STALL_FUNIT
    RDSYS R3, #CPU, #STALL_LOAD
    RDSYS R4, #CPU, #STALL_STORE
    LDW  R12, [R9]
    LDW  R12, [R9]
    LDW  R12, [R9]
    LDW  R12, [R9]
    RDSYS R5, #CPU, #STALL_FUNIT
    RDSYS R6, #CPU, #STALL_LOAD
    RDSYS R7, #CPU, #STALL_STORE
    CMP  R5, R2
    BNE  fail                         ; FUNIT moved during a LOAD burst
    CMP  R7, R4
    BNE  fail                         ; STORE moved during a LOAD burst
    CMP  R6, R3
    BEQ  fail                         ; LOAD did not move on a LOAD burst

    LLI  R1, #1               ; every isolation + positive check passed → PASS
fail:
    BREAK
