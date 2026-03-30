; test_iret.s — WRSPR EPC: redirect execution after a fault
;
; Simulates a simplified context switch:
;   1. Enable MMU with page 0 identity-mapped
;   2. Access unmapped page → TLB miss trap
;   3. Handler reads EPC and ESR via RDSPR
;   4. Verifies EPC points at the faulting instruction
;   5. Uses WRSPR EPC + WRSPR ESR to set a different return context,
;      then ERET to jump there
;
; This proves the kernel can redirect execution after a fault —
; the core mechanism behind "kill the process" or "switch to another process."
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9

; ═══════════════════════════════════════════════════════════════
; TLB miss handler — reads exception regs, ERETs to alternate_entry
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    ; Read and verify EPC = address of the faulting LDW
    RDSPR R8, EPC
    CMP   R8, R12             ; R12 was set to faulting instruction address
    BNE   fail

    ; Read ESR — should have S=1 (we were in supervisor mode)
    RDSPR R9, ESR

    ; Construct target SR: supervisor mode, interrupts disabled
    LLI   R10, #0
    LUI   R10, #0x8000        ; SR.S=1 (bit 31), SR.I=0
    WRSPR ESR, R10

    ; Set return address to alternate_entry (not the faulting instruction)
    LA    R11, #alternate_entry
    WRSPR EPC, R11

    ; ERET returns via modified EPC/ESR
    ERET

    ; Should never reach here
    B     fail

; ═══════════════════════════════════════════════════════════════
; Alternate entry — ERET target (proves we redirected execution)
; ═══════════════════════════════════════════════════════════════
alternate_entry:
    LLI  R1, #1               ; PASS — we got here via ERET
    BREAK

; ═══════════════════════════════════════════════════════════════
; Main test
; ═══════════════════════════════════════════════════════════════
_start:
    LLI  R1, #0               ; assume fail

    ; ── Install vector table in RAM ─────────────────────────
    LA   R2, #tlb_miss_handler
    STW  R2, [R0 + #8]        ; vector[2] = TLB miss (offset 0x08)

    ; ── Map VPN 0 → PPN 0 (code page) ────────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map ROM page (VPN 0xFFFFE → PPN 0xFFFFE) ───────────
    LLI  R2, #30
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFFE00
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFFE0B9
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ────────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Save address of the faulting LDW for handler to verify ─
    LA   R12, #fault_ldw

    ; ── Access unmapped page → fault ──────────────────────────
    LLI  R5, #0x3000
fault_ldw:
    LDW  R6, [R5]             ; THIS FAULTS — handler runs, ERETs elsewhere

    ; If we reach here, ERET didn't redirect
    B    fail

fail:
    BREAK
