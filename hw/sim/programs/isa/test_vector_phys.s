; test_vector_phys.s — Verify vector table is read from physical addresses
; REQUIRES: mmu-d mmu-i
;
; Proves the vector table read bypasses the MMU by:
;   1. Writing a TLB miss handler address to vector[2] in RAM
;   2. Mapping page 0 (vector table) and ROM page in TLB
;   3. Enabling MMU
;   4. Invalidating page 0's TLB entry (vector table now unmapped)
;   5. Triggering a TLB miss (access unmapped page 3)
;   6. CPU reads handler address from physical 0x08 (bypassing MMU)
;   7. If bypass works, handler runs; if not, the read faults (infinite loop)
;
; With address-based vectors (MIPS-style), the CPU reads a handler
; ADDRESS from the vector table. This read must bypass the MMU even
; when the vector table's virtual page is unmapped.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9

_start:
    LLI  R1, #0

    ; ── Install TLB miss handler in vector table ─────────────
    LA   R2, #miss_handler
    STW  R2, [R0 + #8]        ; vector[2] = TLB miss handler

    ; ── Map page 0 (VPN 0 → PPN 0) for vector table access ──
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map ROM page (VPN 0xFFFF0 → PPN 0xFFFF0) ────────────
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ───────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Invalidate page 0 (unmap the vector table page) ──────
    ; After this, virtual address 0x00 is unmapped. But the
    ; PHYSICAL vector table at 0x00 is still in RAM.
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    WRSYS R2, #MMU, #TLB_PTE    ; PTE=0 → V=0 → invalid

    ; ── Trigger TLB miss (page 3, unmapped) ──────────────────
    ; CPU will read handler address from physical 0x08 (bypass MMU).
    ; If it goes through the MMU, page 0 is unmapped → nested fault.
    LLI  R5, #0x3000
    LDW  R6, [R5]              ; FAULTS → TLB miss

    ; Should never reach here — handler breaks
    B    fail

fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; TLB miss handler — if we get here, the vector bypass worked
; ═══════════════════════════════════════════════════════════════
miss_handler:
    ; Verify it was a real TLB miss at 0x3000
    RDSYS R8, #MMU, #FAULT_ADDR
    LLI   R9, #0x3000
    CMP   R8, R9
    BNE   fail

    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F
    AND   R8, R9
    CMP   R8, #1               ; FAULT_TLB_MISS
    BNE   fail

    ; PASS — physical vector table read bypass confirmed
    LLI  R1, #1
    BREAK
