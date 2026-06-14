; test_bus_fault.s — Bus fault: verify exception on unmapped address
; REQUIRES: mmu bus-fault wrspr
;
; Tests:
;   1. Install bus fault handler at vector 0 (VEC_BUS_FAULT)
;   2. With MMU disabled (bypass), load from unmapped address 0x02000000
;      (above 16 MB RAM — no device claims this address)
;   3. CPU should trap: EPC/ESR saved, dispatch to vector 0 (0x00)
;   4. Fault handler verifies FAULT_ADDR and FAULT_STATUS
;   5. Also tests store to unmapped address
;
; Result: R1=1 PASS, R1=0 FAIL

; Unmapped address: 32 MB — above the 16 MB RAM, below I/O space
.equ UNMAPPED, 0x02000000

_start:
    LLI  R1, #0               ; assume fail

    ; ── Install bus fault handler at vector 0 ───────────────
    LA   R2, #bus_fault_handler
    STW  R2, [R0 + #0]        ; vector[0] = bus fault handler

    ; ── Test 1: Load from unmapped address → bus fault ──────
    LLI  R11, #1              ; R11 = test phase (1 = load test)
    LI   R5, #UNMAPPED
    LDW  R6, [R5]             ; THIS SHOULD FAULT
    CMP  R11, #0              ; Bus fault handler clears R11 on success
    BNE  fail

; ── Test 2 entry: store to unmapped address ─────────────────
test_store:
    LLI  R11, #2              ; R11 = test phase (2 = store test)
    LI   R5, #UNMAPPED
    STW  R0, [R5]             ; THIS SHOULD FAULT
    CMP  R11, #0              ; Bus fault handler clears R11 on success
    BNE  fail

    LI   R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Bus fault handler — verify fault info, advance EPC, return
; R11 cleared to 0 on success; left unchanged on failure.
; ═══════════════════════════════════════════════════════════════
bus_fault_handler:
    ; Check FAULT_ADDR == UNMAPPED
    RDSYS R2, #MMU, #FAULT_ADDR
    LI    R3, #0x02000000
    CMP   R2, R3
    BNE   bus_handler_fail

    ; Check fault type [3:0] == FAULT_BUS (4)
    RDSYS R2, #MMU, #FAULT_STATUS
    LI    R3, #0xF
    MOV   R4, R2
    AND   R4, R3
    CMP   R4, #FAULT_BUS
    BNE   bus_handler_fail

    ; Check access type: R/W bits [9:8]
    LI    R3, #0x300
    MOV   R4, R2
    AND   R4, R3
    CMP   R11, #1
    BNE   not_phase1
    CMP   R4, #FSTAT_R
    BNE   bus_handler_fail
    B     bus_handler_success
not_phase1:
    CMP   R11, #2
    BNE   bus_handler_fail
    CMP   R4, #FSTAT_W
    BNE   bus_handler_fail

bus_handler_success:
    LLI   R11, #0
bus_handler_fail:
    RDSPR R2, EPC
    ADD   R2, #4
    WRSPR EPC, R2
    ERET
