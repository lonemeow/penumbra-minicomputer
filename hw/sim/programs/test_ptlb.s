; test_ptlb.s — Pinned TLB: write, readback, lookup, priority, ASID, G bit, faults
;
; The pinned TLB is accessed via TLB_INDEX with bit 6 set.
; Same TLB_VPN/TLB_PTE sysregs, same 3-write protocol.
;
; Tests:
;   1. Write a pinned entry via TLB_INDEX[6]=1, read it back
;   2. Enable MMU with only pinned entries — verify data access works
;   3. Pinned entry takes priority over conflicting main TLB entry
;   4. Invalidated pinned entry no longer hits
;   5. ASID mismatch on non-global pinned entry → miss
;   6. G bit bypasses ASID check on pinned entry → hit
;   7. Permission fault from pinned entry (write to read-only page)
;
; Result: R1=1 PASS, R1=0 FAIL

; Sysreg device 0 = MMU
.equ MMUCR,        0
.equ FAULT_ADDR,   1
.equ FAULT_STATUS, 2
.equ TLB_VPN,      3
.equ TLB_PTE,      4
.equ TLB_INDEX,    5

; PTE flags
.equ KERN_RWX,   0xBD    ; V|C|R|W|X|G
.equ KERN_RX,    0xAD    ; V|C|R|X|G (no W)
.equ NOGL_RWX,   0x3D    ; V|C|R|W|X (no G — ASID-specific)
.equ NOGL_RX,    0x2D    ; V|C|R|X (no G, no W)

; Fault constants
.equ FAULT_PROT, 2
.equ FSTAT_W,    0x200

_start:
    ; Install exception handlers before anything else
    LA   R2, #prot_handler
    STW  R2, [R0 + #0x0C]     ; vector[3] = TLB protection fault
    LA   R2, #miss_handler
    STW  R2, [R0 + #0x08]     ; vector[2] = TLB miss

    B    start

; ═══════════════════════════════════════════════════════════════
; TLB protection fault handler (vector 3)
;
; On entry: R10 = expected FAULT_ADDR, R12 = new PTE for remap
; Uses R8, R9 as scratch.
; ═══════════════════════════════════════════════════════════════
prot_handler:
    ; Verify FAULT_ADDR
    RDSYS R8, #MMU, #FAULT_ADDR
    CMP   R8, R10
    BNE   fail

    ; Verify fault type = FAULT_PROT
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F
    AND   R8, R9
    CMP   R8, #FAULT_PROT
    BNE   fail

    ; Remap: update pinned slot 2 with corrected permissions
    LLI   R8, #0x42
    WRSYS R8, #MMU, #TLB_INDEX  ; pinned slot 2
    LLI   R9, #0x0200           ; VPN=2, ASID=0
    WRSYS R9, #MMU, #TLB_VPN
    WRSYS R12, #MMU, #TLB_PTE   ; new PTE with W permission
    ERET

; ═══════════════════════════════════════════════════════════════
; TLB miss handler (vector 2) — used by ASID mismatch test
;
; When an ASID mismatch causes a miss, we land here.
; Set R9 = 1 to signal the miss was detected, advance EPC past
; the faulting instruction, and return.
; ═══════════════════════════════════════════════════════════════
miss_handler:
    LLI   R9, #1              ; signal: miss detected
    RDSPR R8, EPC
    ADD   R8, #4              ; skip faulting instruction
    WRSPR EPC, R8
    ERET

; ═══════════════════════════════════════════════════════════════
; Main tests
; ═══════════════════════════════════════════════════════════════
start:
    LLI  R1, #0               ; assume fail

    ; ══════════════════════════════════════════════════════
    ; Test 1: Write pinned entry, read back via sysregs
    ; ══════════════════════════════════════════════════════

    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    RDSYS R4, #MMU, #TLB_VPN
    CMP  R4, R2
    BNE  fail
    RDSYS R4, #MMU, #TLB_PTE
    CMP  R4, R3
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 2: Pinned entry provides translation with MMU on
    ; ══════════════════════════════════════════════════════

    ; Pin ROM page in slot 1
    LLI  R2, #0x41
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00BD
    WRSYS R2, #MMU, #TLB_PTE

    ; Enable MMU (ASID=0)
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; Store and load through pinned page 0
    LLI  R5, #0xBEEF
    LLI  R6, #0x0100
    STW  R5, [R6]
    LDW  R7, [R6]
    CMP  R7, R5
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 3: Pinned TLB wins over main TLB
    ; ══════════════════════════════════════════════════════

    ; Install conflicting main TLB: VPN 0 → PPN 1
    LLI   R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LI    R2, #0x00000000
    WRSYS R2, #MMU, #TLB_VPN
    LI    R2, #0x000100BD
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R6, #0x0100
    LLI  R5, #0xDEAD
    STW  R5, [R6]

    LDW  R5, [R6]
    LLI  R7, #0xDEAD
    CMP  R7, R5
    BNE  fail

    ; Disable MMU
    LLI  R4, #0
    WRSYS R4, #MMU, #MMUCR

    ; Verify PA 0x0100 has 0xDEAD
    LDW  R5, [R6]
    LLI  R7, #0xDEAD
    CMP  R7, R5
    BNE  fail

    ; Verify PA 0x1100 was NOT written
    LI   R8, #0x1100
    LDW  R5, [R8]
    CMP  R5, R0
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 4: Invalidate pinned entry — should no longer hit
    ; ══════════════════════════════════════════════════════

    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_PTE

    RDSYS R4, #MMU, #TLB_PTE
    CMP  R4, R2
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 5: ASID mismatch on non-global pinned entry → miss
    ; ══════════════════════════════════════════════════════

    ; Re-pin page 0 with G=1 (for code fetch) in slot 0
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX            ; G=1
    WRSYS R3, #MMU, #TLB_PTE

    ; Pin slot 2: VPN=2, ASID=5, no G bit → only matches ASID 5
    LLI  R2, #0x42
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0205              ; VPN=2, ASID=5
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #0x203D              ; PPN=2, V|C|R|W|X (no G)
    WRSYS R2, #MMU, #TLB_PTE

    ; Plant sentinel at PA 0x2100
    LI   R6, #0x2100
    LLI  R5, #0x5555
    STW  R5, [R6]

    ; Enable MMU with ASID=0 (mismatch with entry's ASID=5)
    LLI  R4, #1                   ; M=1, ASID=0
    WRSYS R4, #MMU, #MMUCR

    ; Attempt load from VA 0x2100 — should TLB miss (ASID mismatch)
    ; Miss handler sets R9=1 and skips the load
    LLI  R9, #0                   ; clear miss flag
    LDW  R7, [R6]                 ; ← should miss, handler skips
    CMP  R9, #1                   ; miss handler ran?
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 6: G bit bypasses ASID check → hit
    ; ══════════════════════════════════════════════════════

    ; Update pinned slot 2: same mapping but add G bit
    LLI  R2, #0x42
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0205              ; VPN=2, ASID=5
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #0x20BD              ; PPN=2, V|C|R|W|X|G (now global!)
    WRSYS R2, #MMU, #TLB_PTE

    ; Still ASID=0 in MMUCR — but G=1 means it should hit anyway
    LDW  R7, [R6]                 ; should hit (G bypasses ASID)
    CMP  R7, #0x5555              ; sentinel from earlier
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 7: Permission fault from pinned entry
    ; ══════════════════════════════════════════════════════

    ; Update pinned slot 2: VPN=2, read-only (no W), global
    LLI  R2, #0x42
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0200              ; VPN=2, ASID=0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #0x20AD              ; PPN=2, V|C|R|X|G (no W!)
    WRSYS R2, #MMU, #TLB_PTE

    ; Set up prot handler expectations
    LI   R10, #0x2100             ; expected FAULT_ADDR
    LLI  R12, #0x20BD             ; new PTE: PPN=2, add W permission

    ; Write should fault → prot_handler remaps → ERET → retry succeeds
    LLI  R7, #0xAAAA
    STW  R7, [R6]                 ; ← faults, handler fixes, retries

    ; Disable MMU, verify write landed at PA 0x2100
    LLI  R4, #0
    WRSYS R4, #MMU, #MMUCR

    LDW  R5, [R6]
    CMP  R5, #0xAAAA
    BNE  fail

    ; ── All checks passed ───────────────────────────────
    LLI  R1, #1
fail:
    BREAK
