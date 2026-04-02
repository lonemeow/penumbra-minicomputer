; test_bus_fault_mmu.s — Bus fault with MMU enabled (device probing pattern)
;
; Simulates the NetBSD bus_space_peek pattern: map a virtual page to
; an unmapped physical address (potential device MMIO), attempt a read,
; and catch the bus fault. This is how the kernel probes for optional
; hardware at boot.
;
; Tests:
;   1. Map page 0 (vector table + data), ROM page (code)
;   2. Map VPN 5 (0x5000) → PPN 0x02000 (phys 0x02000000, unmapped)
;   3. Enable MMU, install bus fault handler
;   4. Read from 0x5000 → TLB hit, phys addr unmapped → bus fault
;   5. Verify FAULT_ADDR = 0x5000 (virtual, not physical)
;   6. Verify FAULT_STATUS type = FAULT_BUS, access = read
;   7. Also test write to same mapped-but-unmapped page
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX,  0xB9         ; V|R|W|X|G — kernel code+data page
.equ KERN_RW,   0x99         ; V|R|W|G   — kernel data, no exec, uncached
.equ PROBE_VA,  0x5000       ; Virtual address to probe (VPN 5)

_start:
    LLI  R1, #0               ; assume fail

    ; ── Install bus fault handler at vector 0 ───────────────
    LA   R2, #bus_fault_handler
    STW  R2, [R0 + #0]        ; vector[0] = bus fault handler

    ; ── Map page 0 → PPN 0 (vector table, data, stack) ─────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map ROM page (VPN 0xFFFF0 → PPN 0xFFFF0) ───────────
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00B9
    WRSYS R2, #MMU, #TLB_PTE

    ; ── Map VPN 5 → PPN 0x02000 (phys 0x02000000, unmapped) ──
    ; TLB translates successfully, but physical address hits no device.
    LLI  R2, #5
    WRSYS R2, #MMU, #TLB_INDEX  ; set 5, way 0
    LLI  R2, #0x0500            ; {4'b0, VPN=0x00005, ASID=0x00} → 0x0000_0500
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0x02000099        ; {PPN=0x02000, SW=0x00, flags=KERN_RW}
    WRSYS R2, #MMU, #TLB_PTE

    ; ── Enable MMU ──────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Test 1: Read from mapped-but-unmapped page ──────────
    LLI  R11, #1              ; phase 1 = read probe
    LLI  R5, #PROBE_VA
    LDW  R6, [R5]             ; TLB hit → phys 0x02000000 → BUS FAULT
    CMP  R11, #0
    BNE  fail

    ; ── Test 2: Write to mapped-but-unmapped page ───────────
    LLI  R11, #2              ; phase 2 = write probe
    LLI  R5, #PROBE_VA
    STW  R0, [R5]             ; TLB hit → phys 0x02000000 → BUS FAULT
    CMP  R11, #0
    BNE  fail

    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Bus fault handler — verify fault info, advance EPC, return
; ═══════════════════════════════════════════════════════════════
bus_fault_handler:
    ; Check FAULT_ADDR == PROBE_VA (virtual address, not physical!)
    RDSYS R2, #MMU, #FAULT_ADDR
    LLI   R3, #PROBE_VA
    CMP   R2, R3
    BNE   handler_fail

    ; Check fault type == FAULT_BUS (4)
    RDSYS R2, #MMU, #FAULT_STATUS
    LLI   R3, #0xF
    MOV   R4, R2
    AND   R4, R3
    CMP   R4, #FAULT_BUS
    BNE   handler_fail

    ; Check access type matches phase
    LI    R3, #0x300
    MOV   R4, R2
    AND   R4, R3
    CMP   R11, #1
    BNE   check_write
    CMP   R4, #FSTAT_R
    BNE   handler_fail
    B     handler_ok
check_write:
    CMP   R4, #FSTAT_W
    BNE   handler_fail

handler_ok:
    LLI   R11, #0             ; signal success to caller
handler_fail:
    RDSPR R2, EPC
    ADD   R2, #4
    WRSPR EPC, R2
    ERET
