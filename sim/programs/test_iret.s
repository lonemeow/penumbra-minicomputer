; test_iret.s — IRET: return to a different context than the one that faulted
;
; Simulates a simplified context switch:
;   1. Enable MMU with page 0 identity-mapped
;   2. Access unmapped page → TLB miss trap
;   3. Handler reads shadow_PC and shadow_SR via RDSPC
;   4. Verifies shadow_PC points at the faulting instruction
;   5. Uses IRET to jump to a DIFFERENT address (not the faulting one)
;      with a constructed SR value
;
; This proves the kernel can redirect execution after a fault —
; the core mechanism behind "kill the process" or "switch to another process."
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9

; ═══════════════════════════════════════════════════════════════
; Vector table
; ═══════════════════════════════════════════════════════════════
.org 0x00
    B    start              ; 0x00: reset
    B    fail               ; 0x04: IRQ
    B    tlb_miss_handler   ; 0x08: TLB miss
    B    fail               ; 0x0C: protection fault

; ═══════════════════════════════════════════════════════════════
; TLB miss handler — reads shadow regs, IRETs to alternate_entry
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    ; Read and verify shadow_PC = address of the faulting LDW
    RDSPC R8, SPC
    CMP   R8, R12             ; R12 was set to faulting instruction address
    BNE   fail

    ; Read shadow_SR — should have S=1 (we were in supervisor mode)
    RDSPC R9, SSR

    ; Construct target SR: supervisor mode, interrupts disabled
    ; (same as current state — we just want to prove IRET works)
    LLI   R10, #0
    LUI   R10, #0x8000        ; SR.S=1 (bit 31), SR.I=0

    ; Load alternate_entry address
    LLI   R11, alternate_entry

    ; IRET to alternate_entry with constructed SR
    IRET  R10, R11

    ; Should never reach here
    B     fail

; ═══════════════════════════════════════════════════════════════
; Alternate entry — IRET target (proves we redirected execution)
; ═══════════════════════════════════════════════════════════════
alternate_entry:
    LLI  R1, #1               ; PASS — we got here via IRET
    B    halt

; ═══════════════════════════════════════════════════════════════
; Main test
; ═══════════════════════════════════════════════════════════════
start:
    LLI  R1, #0               ; assume fail

    ; ── Map VPN 0 → PPN 0 (code page) ────────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ────────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Save address of the faulting LDW for handler to verify ─
    LLI  R12, fault_ldw

    ; ── Access unmapped page → fault ──────────────────────────
    LLI  R5, #0x3000
fault_ldw:
    LDW  R6, [R5]             ; THIS FAULTS — handler runs, IRETs elsewhere

    ; If we reach here, IRET didn't redirect
    B    fail

fail:
    LLI  R1, #0
halt:
    B    halt
