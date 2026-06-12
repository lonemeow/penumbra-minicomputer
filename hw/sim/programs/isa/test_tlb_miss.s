; test_tlb_miss.s — TLB miss trap: verify fault handler runs on unmapped access
; REQUIRES: mmu-d
;
; Tests:
;   1. Install TLB miss handler in vector table
;   2. Identity-map page 0 (low data) — VPN 0 → PPN 0
;   3. Enable MMU
;   4. Load from address 0x3000 (VPN 3) — NOT mapped in TLB
;   5. CPU should trap: EPC/ESR saved, dispatch to vector 2 (0x08)
;   6. Fault handler verifies FAULT_ADDR = 0x3000, FAULT_STATUS shows TLB miss
;   7. Handler sets R1=1 (PASS) and breaks
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9          ; V|R|W|X|G — kernel code+data page

_start:
    LLI  R1, #0               ; assume fail

    ; ── Install vector table in RAM ─────────────────────────
    LA   R2, #tlb_miss_handler
    STW  R2, [R0 + #8]        ; vector[2] = TLB miss handler

    ; ── Map VPN 0 → PPN 0 (identity, for vector table + data) ──
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX  ; select slot 0
    WRSYS R2, #MMU, #TLB_VPN    ; VPN=0, ASID=0
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE    ; PPN=0, flags=V|R|W|X|G

    ; ── Map ROM page (VPN 0xFFFF0 → PPN 0xFFFF0) ───────────
    ; Set index = VPN[4:0] = 0x1E = 30, way 0 → TLB_INDEX = 30
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000        ; VPN = 0xFFFF0
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9        ; PPN = 0xFFFF0, KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ──────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR      ; M=1 → MMU on

    ; ── Access unmapped address → should trap ───────────────
    ; VPN 3 (address 0x3000) has NO TLB entry → TLB miss
    LLI  R5, #0x3000
    LDW  R6, [R5]              ; THIS SHOULD FAULT

    ; If we reach here, the fault didn't fire
fail:
    BREAK

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
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F           ; mask for fault type field
    AND   R8, R9              ; R8 = fault_type = FAULT_STATUS & 0x0F
    CMP  R8, #1              ; 1 = FAULT_TLB_MISS
    BNE   fail

    ; PASS — fault handler ran with correct fault address
    LLI   R1, #1
    BREAK
