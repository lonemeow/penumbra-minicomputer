; test_rti.s — TLB miss → handler fills TLB → RTI → faulting load completes
;
; This is the core demand-paging loop: an unmapped access faults, the
; handler installs the mapping, and RTI restarts the faulting instruction
; which now succeeds through the TLB.
;
; Setup:
;   - Identity-map page 0 (code + low data)
;   - Store sentinel 0xFACE at physical 0x1000 (page 1) while MMU is off
;   - Enable MMU — page 1 is NOT mapped
;   - Load from 0x1000 → TLB miss → handler maps VPN 1 → PPN 1 → RTI
;   - Restarted load succeeds, gets 0xFACE
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9          ; V|R|W|X|G

; ═══════════════════════════════════════════════════════════════
; Vector table
; ═══════════════════════════════════════════════════════════════
.org 0x00
    B    start              ; 0x00: reset
    B    fail               ; 0x04: IRQ — unexpected
    B    tlb_miss_handler   ; 0x08: TLB miss
    B    fail               ; 0x0C: protection fault — unexpected

; ═══════════════════════════════════════════════════════════════
; TLB miss handler — maps VPN 1 → PPN 1, then returns
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    ; Install mapping: VPN 1 → PPN 1 in TLB slot 1
    LLI  R10, #1
    WRSYS R10, #MMU, #TLB_INDEX   ; slot 1 (set=1, way=0)
    LLI  R11, #0x0100             ; VPN word: VPN=1 (bits[27:8]), ASID=0
    WRSYS R11, #MMU, #TLB_VPN
    LLI  R11, #0x10B9             ; PTE: PPN=1 (bits[31:12]=0x00001), flags=KERN_RWX
    WRSYS R11, #MMU, #TLB_PTE     ; commits entry

    ; Return to faulting instruction (LDW at 0x1000 will now hit)
    RTI

; ═══════════════════════════════════════════════════════════════
; Main test
; ═══════════════════════════════════════════════════════════════
start:
    LLI  R1, #0                ; assume fail

    ; ── Plant sentinel at physical 0x1000 (MMU off = bypass) ──
    LLI  R2, #0xFACE
    LLI  R3, #0x1000
    STW  R2, [R3]              ; mem[0x1000] = 0xFACE

    ; ── Map VPN 0 → PPN 0 (code page, identity) ──────────────
    LLI  R4, #0
    WRSYS R4, #MMU, #TLB_INDEX
    WRSYS R4, #MMU, #TLB_VPN
    LLI  R5, #KERN_RWX
    WRSYS R5, #MMU, #TLB_PTE

    ; ── Enable MMU (page 1 intentionally NOT mapped) ─────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; ── Load from unmapped page 1 → TLB miss → handler → RTI ─
    LDW  R7, [R3]             ; faults, handler maps VPN 1, RTI restarts

    ; ── If we reach here, the restart succeeded ───────────────
    CMP  R7, R2               ; R7 should be 0xFACE
    BNE  fail

    ; ── Disable MMU ──────────────────────────────────────────
    LLI  R6, #0
    WRSYS R6, #MMU, #MMUCR

    ; PASS
    LLI  R1, #1
fail:
halt:
    B    halt
