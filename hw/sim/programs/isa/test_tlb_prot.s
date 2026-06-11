; test_tlb_prot.s — TLB protection fault: multiple permission scenarios
; REQUIRES: mmu
;
; Tests (all in supervisor mode, MMU enabled):
;   1. Write to read-only page → prot fault, handler remaps with W, ERET retries
;   2. Read from write-only page → prot fault, handler remaps with R, ERET retries
;
; Each fault handler invocation verifies FAULT_ADDR and FAULT_STATUS,
; remaps the page with correct permissions, and ERETs to retry.
;
; Register convention:
;   R1        = pass/fail result
;   R2-R6     = main program scratch (setup)
;   R7        = data for STW / result from LDW (must not be clobbered)
;   R10       = expected FAULT_ADDR (set before fault, read by handler)
;   R11       = expected access bit (FSTAT_R or FSTAT_W, set before fault)
;   R12       = new PTE for remap (set before fault)
;   R8,R9,R13 = handler scratch (safe — not used by faulting instructions)
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX,  0xB9       ; V|R|W|X|G — full access
.equ FAULT_PROT, 2         ; FAULT_STATUS[3:0]
.equ FSTAT_R,   0x100      ; bit [8] = read access
.equ FSTAT_W,   0x200      ; bit [9] = write access

; ═══════════════════════════════════════════════════════════════
; Entry point — set up vectors at runtime, then run tests
; ═══════════════════════════════════════════════════════════════
_start:
    ; Install TLB protection fault handler (VEC_TLB_PROT = 3, offset 0x0C)
    LA   R2, #prot_handler
    STW  R2, [R0 + #0x0C]

    B    start

; ═══════════════════════════════════════════════════════════════
; Protection fault handler
;
; Uses only R8, R9, R13 as scratch — must not touch R7 (STW/LDW operand)
; Inputs (set by main program before faulting instruction):
;   R10 = expected FAULT_ADDR
;   R11 = expected access bit mask (FSTAT_R or FSTAT_W)
;   R12 = new PTE to install
; ═══════════════════════════════════════════════════════════════
prot_handler:
    ; Verify FAULT_ADDR matches expected
    RDSYS R8, #MMU, #FAULT_ADDR
    CMP   R8, R10
    BNE   fail

    ; Verify fault type = FAULT_PROT (bits [3:0] = 2)
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F
    MOV   R13, R8
    AND   R13, R9
    CMP  R13, #FAULT_PROT
    BNE   fail

    ; Verify expected access type bit is set
    MOV   R13, R8
    AND   R13, R11
    CMP   R13, R11
    BNE   fail

    ; Remap page 1 with corrected permissions
    LLI   R8, #1
    WRSYS R8, #MMU, #TLB_INDEX
    LLI   R9, #0x0100           ; VPN=1, ASID=0
    WRSYS R9, #MMU, #TLB_VPN
    WRSYS R12, #MMU, #TLB_PTE

    ; Return to retry the faulting instruction
    ERET

; ═══════════════════════════════════════════════════════════════
; Main test
; ═══════════════════════════════════════════════════════════════
start:
    LLI  R1, #0

    ; ── Map VPN 0 → PPN 0 (code page, full access) ───────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Plant sentinel at physical 0x1000 (before MMU on) ─────
    LLI  R4, #0xCAFE
    LLI  R5, #0x1000
    STW  R4, [R5]              ; mem[0x1000] = 0xCAFE

    ; ══════════════════════════════════════════════════════════
    ; Test 1: Write to read-only page
    ; ══════════════════════════════════════════════════════════

    ; Map VPN 1 → PPN 1, read-only (R+X, no W)
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10A9            ; PTE: PPN=1, V|R|X|G (no W!)
    WRSYS R3, #MMU, #TLB_PTE

    ; Map ROM page (VPN 0xFFFF0 → PPN 0xFFFF0)
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; Enable MMU
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; Read should succeed (has R permission)
    LDW  R7, [R5]
    CMP  R7, R4                ; should be 0xCAFE
    BNE  fail

    ; Set up handler expectations for write fault
    LLI  R10, #0x1000          ; expected FAULT_ADDR
    LLI  R11, #FSTAT_W         ; expected access = write
    LLI  R12, #0x10B9          ; new PTE: PPN=1, V|R|W|X|G

    ; Write should fault → handler remaps → ERET → retry succeeds
    LLI  R7, #0xBEEF
    STW  R7, [R5]              ; FAULTS then retries

    ; Verify write landed (turn off MMU for direct read)
    LLI  R6, #0
    WRSYS R6, #MMU, #MMUCR
    LDW  R7, [R5]
    CMP R7, #0xBEEF
    BNE  fail

    ; ══════════════════════════════════════════════════════════
    ; Test 2: Read from write-only page
    ; ══════════════════════════════════════════════════════════

    ; Remap VPN 1 → PPN 1, write-only (W+X, no R)
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10B1            ; PTE: PPN=1, V|W|X|G (no R!)
    WRSYS R3, #MMU, #TLB_PTE

    ; MMU back on
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; Set up handler expectations for read fault
    LLI  R10, #0x1000          ; expected FAULT_ADDR
    LLI  R11, #FSTAT_R         ; expected access = read
    LLI  R12, #0x10B9          ; new PTE: PPN=1, V|R|W|X|G

    ; Read should fault → handler remaps → ERET → retry succeeds
    LDW  R7, [R5]              ; FAULTS then retries

    ; Verify read got the right value
    CMP R7, #0xBEEF
    BNE  fail

    ; ── All checks passed ────────────────────────────────────
    LLI  R6, #0
    WRSYS R6, #MMU, #MMUCR     ; MMU off
    LLI  R1, #1
fail:
    BREAK
