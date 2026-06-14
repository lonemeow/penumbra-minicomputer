; test_bus_fault_fetch.s — Bus fault on an instruction fetch
; REQUIRES: mmu bus-fault wrspr
;
; A fetch from an address no device claims must trap to VEC_BUS_FAULT
; rather than execute garbage. With the MMU bypassed, JMP to a word-
; aligned unmapped address; the fetch's bus fault reports the faulting
; PC in EPC and FAULT_ADDR, with an execute (FSTAT_X) access type.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ UNMAPPED, 0x02000000      ; word-aligned, at the 32 MB RAM top — no device

_start:
    LLI  R1, #0                ; assume fail

    ; ── Install bus fault handler at vector 0 (VEC_BUS_FAULT) ───
    LA   R2, #bus_fault_handler
    STW  R2, [R0 + #0]

    ; ── Jump to the unmapped address — the fetch should bus-fault ──
    LI   R6, #UNMAPPED
    JMP  R6

    ; Handler ERETs here on success (it cannot return to the faulting PC)
recovered:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Bus fault handler — verify the fetch-fault info, recover via ERET.
; R1 is left 0 on any mismatch, set to 1 only when every field checks.
; ═══════════════════════════════════════════════════════════════
bus_fault_handler:
    ; EPC == the faulting fetch PC
    RDSPR R2, EPC
    LI    R3, #UNMAPPED
    CMP   R2, R3
    BNE   handler_done

    ; FAULT_ADDR == the faulting PC (an IF-side fault reports its own PC)
    RDSYS R2, #MMU, #FAULT_ADDR
    CMP   R2, R3
    BNE   handler_done

    ; FAULT_STATUS type [3:0] == FAULT_BUS
    RDSYS R4, #MMU, #FAULT_STATUS
    MOV   R2, R4
    LLI   R3, #0xF
    AND   R2, R3
    CMP   R2, #FAULT_BUS
    BNE   handler_done

    ; Access type [10:8] == execute (ACC_EXEC=0b100 → bit FSTAT_X set)
    MOV   R2, R4
    LLI   R3, #0x700
    AND   R2, R3
    LLI   R3, #0x400
    CMP   R2, R3
    BNE   handler_done

    LLI   R1, #1               ; PASS — all fields match

handler_done:
    ; The faulting PC is unmapped, so returning there re-faults forever;
    ; redirect EPC to `recovered` and ERET to a real instruction.
    LA    R2, #recovered
    WRSPR EPC, R2
    ERET
