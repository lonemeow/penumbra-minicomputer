; test_vector_phys.s — Verify exception vectors are at physical addresses
;
; Proves the vector table fetch bypasses the MMU by unmapping page 0
; (where the vector table lives) and then triggering a TLB miss.
; If the vector fetch at 0x08 goes through the MMU, it would fail
; (page 0 unmapped). If it bypasses the MMU, the physical vector
; entry is fetched correctly and branches to the handler on page 2.
;
; Memory layout:
;   Page 0 (0x0000): vector table only — branches to page 2 handlers
;   Page 1 (0x1000): main test code — unmaps page 0, triggers fault
;   Page 2 (0x2000): handlers — verify fault, BREAK
;   Page 3 (0x3000): unmapped — access here triggers TLB miss
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9

; ═══════════════════════════════════════════════════════════════
; Page 0 — Vector table (physical 0x0000)
; These are fetched via physical bypass after an exception.
; ═══════════════════════════════════════════════════════════════
.org 0x00
    B    setup              ; 0x00: reset → setup (on page 0, before remap)
    B    fail_p2            ; 0x04: IRQ → fail
    B    miss_handler_p2    ; 0x08: TLB miss → handler on page 2
    B    fail_p2            ; 0x0C: protection fault → fail

; Setup runs on page 0 (identity mapped initially)
setup:
    LLI  R1, #0

    ; ── Map VPN 0 → PPN 0 (page 0, identity, for setup code) ─
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX  ; slot 0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map VPN 1 → PPN 1 (page 1, main test code) ───────────
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX  ; slot 1
    LLI  R3, #0x0100            ; VPN=1
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10B9            ; PPN=1, KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map VPN 2 → PPN 2 (page 2, handler code) ─────────────
    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX  ; slot 2
    LLI  R3, #0x0200            ; VPN=2
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x20B9            ; PPN=2, KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ────────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Jump to main test code on page 1 ──────────────────────
    LLI  R2, #main_test
    JMP  R2

; ═══════════════════════════════════════════════════════════════
; Page 1 — Main test code (physical 0x1000)
; Executing here after MMU is on, VPN 1 → PPN 1 is mapped.
; ═══════════════════════════════════════════════════════════════
.org 0x1000
main_test:
    ; ── Invalidate page 0's TLB entry ─────────────────────────
    ; After this, the vector table at virtual 0x00 is unmapped.
    ; But the PHYSICAL vector table at 0x00 is still there in memory.
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX  ; slot 0
    WRSYS R2, #MMU, #TLB_VPN
    WRSYS R2, #MMU, #TLB_PTE    ; PTE=0 → V=0 → invalid

    ; ── Trigger TLB miss (page 3, unmapped) ───────────────────
    ; This will vector to 0x08 (TLB miss). If the vector fetch
    ; bypasses the MMU, it gets the correct B miss_handler_p2.
    ; If it goes through the MMU, page 0 is unmapped → garbage.
    LLI  R5, #0x3000
    LDW  R6, [R5]              ; FAULTS → handler on page 2

    ; Should never reach here
    B    fail_p1
fail_p1:
    ; Can't BREAK here easily (page 0 unmapped for the BREAK vector).
    ; Just loop — testbench timeout detects failure.
    B    fail_p1

; ═══════════════════════════════════════════════════════════════
; Page 2 — Handler code (physical 0x2000)
; ═══════════════════════════════════════════════════════════════
.org 0x2000
miss_handler_p2:
    ; If we're here, the vector fetch at physical 0x08 worked!
    ; Verify it was a real TLB miss at the expected address.
    RDSYS R8, #MMU, #FAULT_ADDR
    LLI   R9, #0x3000
    CMP   R8, R9
    BNE   fail_p2

    ; Verify fault type = TLB miss (bits [3:0] = 1)
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F
    AND   R8, R9
    CMPI  R8, #1
    BNE   fail_p2

    ; PASS — physical vector bypass confirmed
    LLI  R1, #1
fail_p2:
    BREAK
