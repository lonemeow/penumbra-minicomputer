; test_tlb_miss.s — TLB miss trap: verify fault handler runs on unmapped access
;
; Tests:
;   1. Identity-map page 0 (code + low data) — VPN 0 → PPN 0
;   2. Enable MMU
;   3. Load from address 0x3000 (VPN 3) — NOT mapped in TLB
;   4. CPU should trap: shadow PC/SR saved, dispatch to vector 2 (0x08)
;   5. Fault handler verifies FAULT_ADDR = 0x3000, FAULT_STATUS shows TLB miss
;   6. Handler sets R1=1 (PASS) and halts
;
; The faulting instruction's PC is saved in shadow_PC so the OS could
; refill the TLB and restart. This test just verifies the trap fires.
;
; Result: R1=1 PASS, R1=0 FAIL
;
; Memory layout:
;   0x00: reset vector  → B start
;   0x04: IRQ vector    → B fail        (should not fire)
;   0x08: TLB miss vec  → B tlb_miss_handler
;   0x0C: prot fault    → B fail        (should not fire)

; ═══════════════════════════════════════════════════════════════
; Vector table (addresses 0x00–0x0F)
; ═══════════════════════════════════════════════════════════════
.org 0x00
    B    start              ; 0x00: reset
    B    fail               ; 0x04: IRQ — unexpected
    B    tlb_miss_handler   ; 0x08: TLB miss — this is what we're testing
    B    fail               ; 0x0C: protection fault — unexpected

; ═══════════════════════════════════════════════════════════════
; TLB miss handler
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    ; Read FAULT_ADDR — should be 0x3000
    RDSYS R8, #MMU, #FAULT_ADDR
    LLI   R9, #0x3000
    CMP   R8, R9
    BNE   fail

    ; Read FAULT_STATUS — check fault type is TLB miss (bits [3:0] = 0x1)
    ; FAULT_STATUS format: [11:8]=access info, [3:0]=fault type
    ; TLB miss = 0x1, expect read access bit [8] set → 0x101
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F           ; mask for fault type field
    AND   R8, R9              ; R8 = fault_type = FAULT_STATUS & 0x0F
    CMPI  R8, #1              ; 1 = FAULT_TLB_MISS
    BNE   fail

    ; PASS — fault handler ran with correct fault address
    LLI   R1, #1
    B     halt

; ═══════════════════════════════════════════════════════════════
; Main test code
; ═══════════════════════════════════════════════════════════════

.equ KERN_RWX, 0xB9          ; V|R|W|X|G — kernel code+data page

start:
    LLI  R1, #0               ; assume fail

    ; ── Map VPN 0 → PPN 0 (identity, for code execution + low data) ──
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX  ; select slot 0
    WRSYS R2, #MMU, #TLB_VPN    ; VPN=0, ASID=0
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE    ; PPN=0, flags=V|R|W|X|G

    ; ── Enable MMU ──────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR      ; M=1 → MMU on

    ; ── Access unmapped address → should trap ───────────────
    ; VPN 3 (address 0x3000) has NO TLB entry → TLB miss
    LLI  R5, #0x3000
    LDW  R6, [R5]              ; THIS SHOULD FAULT

    ; If we reach here, the fault didn't fire
    B    fail

fail:
    LLI  R1, #0
halt:
    B    halt
