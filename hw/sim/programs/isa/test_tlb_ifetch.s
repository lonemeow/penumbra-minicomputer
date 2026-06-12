; test_tlb_ifetch.s — Instruction fetch TLB miss: verify fault on unmapped code
; REQUIRES: mmu-d mmu-i
;
; Tests:
;   1. Copy target code from ROM to RAM at page 2 (0x2000)
;   2. Map page 0 (vectors+data), ROM page, but NOT page 2
;   3. Enable MMU
;   4. JMP to 0x2000 → instruction fetch should fault (TLB miss)
;   5. Fault handler verifies FAULT_ADDR=0x2000, FAULT_STATUS shows
;      TLB miss with ACC_EXEC, then maps page 2 and ERETs to retry
;   6. Retried fetch succeeds, planted code sets R1=1 and BREAKs
;
; FAULT_STATUS layout for TLB miss on supervisor fetch:
;   {20'b0, user_mode=0, access_type=ACC_EXEC(3'b100), 4'b0, FAULT_TLB_MISS(4'b0001)}
;   = 0x00000401
;
; Register convention:
;   R1  = pass/fail result
;   R2-R6 = setup scratch
;   R8,R9 = handler scratch
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX,    0xB9     ; V|R|W|X|G — kernel full access
.equ TARGET_ADDR, 0x2000   ; page 2 — intentionally unmapped

_start:
    LLI  R1, #0              ; assume fail

    ; ── Install TLB miss handler ────────────────────────────
    LA   R2, #ifetch_miss_handler
    STW  R2, [R0 + #8]       ; vector[2] = TLB miss handler

    ; ── Copy target code from ROM to RAM at 0x2000 ──────────
    LA   R2, #target_code
    LLI  R3, #TARGET_ADDR
    LA   R4, #target_code_end

copy:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy

    ; ── Map page 0 → page 0 (vectors + data) ────────────────
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

    ; ── Page 2 (0x2000) is intentionally NOT mapped ──────────

    ; ── Enable MMU ───────────────────────────────────────────
    LLI  R5, #1
    WRSYS R5, #MMU, #MMUCR

    ; ── Jump to unmapped code → should fetch-fault ───────────
    LI   R6, #TARGET_ADDR
    JMP  R6

    ; If we reach here, the fault didn't fire
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; TLB miss handler for instruction fetch
; ═══════════════════════════════════════════════════════════════
ifetch_miss_handler:
    ; Verify FAULT_ADDR == 0x2000
    RDSYS R8, #MMU, #FAULT_ADDR
    CMP   R8, #TARGET_ADDR
    BNE   fail

    ; Verify FAULT_STATUS == 0x401 (ACC_EXEC + TLB_MISS, supervisor)
    RDSYS R8, #MMU, #FAULT_STATUS
    LI    R9, #0x00000401
    CMP   R8, R9
    BNE   fail

    ; Map VPN 2 → PPN 2 with KERN_RWX
    LLI   R9, #2
    WRSYS R9, #MMU, #TLB_INDEX
    LLI   R9, #0x0200        ; VPN = 2, ASID = 0
    WRSYS R9, #MMU, #TLB_VPN
    LI    R9, #0x20B9         ; PPN = 2, KERN_RWX
    WRSYS R9, #MMU, #TLB_PTE

    ERET

; ═══════════════════════════════════════════════════════════════
; Target code — copied to RAM at 0x2000, then executed after
; the fault handler maps page 2.
; ═══════════════════════════════════════════════════════════════
target_code:
    LLI  R1, #1
    BREAK
target_code_end:
