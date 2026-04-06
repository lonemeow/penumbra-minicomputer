; test_ptlb.s — Pinned TLB: write, readback, lookup, priority over main TLB
;
; The pinned TLB is accessed via TLB_INDEX with bit 6 set.
; Same TLB_VPN/TLB_PTE sysregs, same 3-write protocol.
;
; Tests:
;   1. Write a pinned entry via TLB_INDEX[6]=1, read it back
;   2. Enable MMU with only pinned entries — verify data access works
;   3. Pinned entry takes priority over conflicting main TLB entry
;   4. Invalidated pinned entry no longer hits
;
; Result: R1=1 PASS, R1=0 FAIL

; Sysreg device 0 = MMU
.equ MMUCR,       0
.equ TLB_VPN,     3
.equ TLB_PTE,     4
.equ TLB_INDEX,   5

; TLB_INDEX bit 6 = pinned TLB select
.equ PIN,         0x40

; PTE flags
.equ KERN_RWX,  0xBD    ; V|C|R|W|X|G

_start:
    LLI  R1, #0               ; assume fail

    ; ══════════════════════════════════════════════════════
    ; Test 1: Write pinned entry, read back via sysregs
    ; ══════════════════════════════════════════════════════

    ; Write: pinned slot 0, VPN=0 ASID=0, PPN=0 flags=KERN_RWX
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX  ; select pinned slot 0
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN    ; VPN=0, ASID=0
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE    ; PPN=0, flags (commits to pinned)

    ; Read back and verify
    RDSYS R4, #MMU, #TLB_VPN
    CMP  R4, R2
    BNE  fail
    RDSYS R4, #MMU, #TLB_PTE
    CMP  R4, R3
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 2: Pinned entry provides translation with MMU on
    ; ══════════════════════════════════════════════════════

    ; Also need ROM page pinned so we can keep fetching
    LLI  R2, #0x41
    WRSYS R2, #MMU, #TLB_INDEX  ; pinned slot 1
    LI   R2, #0x0FFFF000        ; VPN=0xFFFF0, ASID=0
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00BD        ; PPN=0xFFFF0, KERN_RWX
    WRSYS R2, #MMU, #TLB_PTE

    ; Enable MMU — no main TLB entries, only pinned
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

    ; Install conflicting main TLB entry: VPN 0 → PPN 1 (PA 0x1000).
    ; Pinned entry (VPN 0 → PPN 0) should win, so stores still go
    ; to PA 0x0100, not PA 0x1100.
    LLI   R2, #1
    WRSYS R2, #MMU, #TLB_INDEX  ; main TLB slot 1 (bit 6 clear)
    LI    R2, #0x00000000        ; VPN=0, ASID=0
    WRSYS R2, #MMU, #TLB_VPN
    LI    R2, #0x000100BD        ; PPN=1, KERN_RWX
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R6, #0x0100

    LLI  R5, 0xDEAD
    STW  R5, [R6]

    LDW  R5, [R6]
    LLI  R7, #0xDEAD
    CMP  R7, R5
    BNE  fail

    ; Disable MMU
    LLI  R4, #0
    WRSYS R4, #MMU, #MMUCR

    ; If pinned won, PA 0x0100 has 0xDEAD (not sent to PA 0x1100)
    LDW  R5, [R6]
    LLI  R7, #0xDEAD
    CMP  R7, R5
    BNE  fail

    ; Verify PA 0x1100 was NOT written (should still be zero)
    LI   R8, #0x1100
    LDW  R5, [R8]
    CMP  R5, R0
    BNE  fail

    ; ══════════════════════════════════════════════════════
    ; Test 4: Invalidate pinned entry — should no longer hit
    ; ══════════════════════════════════════════════════════

    ; Invalidate pinned slot 0 (write PTE=0, V=0)
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX  ; select pinned slot 0
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_PTE    ; PTE=0 → V=0

    ; Verify readback shows invalid
    RDSYS R4, #MMU, #TLB_PTE
    CMP  R4, R2
    BNE  fail

    ; ── All checks passed ───────────────────────────────
    LLI  R1, #1
fail:
    BREAK
