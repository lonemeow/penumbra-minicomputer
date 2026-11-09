; test_wrsys_forward.s — WRSYS writes the value its register holds, however
; closely the instruction producing that value precedes it
; REQUIRES: mmu-d mmu-i cache wrspr
;
; Run from cached code, where fetch keeps up with issue, each WRSYS issues
; one, two or three instructions behind the instruction producing its
; value, while that value is still in flight. Each writes TLB_INDEX (a
; plain register: writing it installs nothing) and reads it back with
; RDSYS.
;   1. Copy the body to RAM page 1 (0x1000); pin page 1, cacheable; map
;      the ROM page
;   2. Enable the MMU and the L1s, and jump to the body
;   3. The body runs twice, the second time from a warm L1-I; each pass
;      must read back every value it wrote
;
; Register convention:
;   R1  = pass/fail result
;   R2  = the value each WRSYS writes
;   R3  = the value read back
;   R4  = filler destination
;   R7  = pass count
;
; Result: R1=1 PASS, R1=0 FAIL

.equ BODY_PA,  0x1000           ; page 1

_start:
    BL   tlb_init                  ; the TLB is undefined after reset
    LLI  R1, #0                 ; assume fail

    ; Copy the body to page 1
    LA   R2, #body
    LLI  R3, #BODY_PA
    LA   R4, #body_end
copy:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy

    ; Pinned slot 0: page 1 identity, cacheable (V|C|R|W|X|G)
    RDSPR R2, SCR3          ; pinned slot 0 (mmu.inc)
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0100            ; VPN = 1, ASID = 0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #0x10BD            ; PPN = 1
    WRSYS R2, #MMU, #TLB_PTE

    ; Main TLB: the ROM page identity, uncacheable as in the other
    ; MMU tests
    LI   R2, #0x0FFFF000        ; TLB_VPN word, way 0
    BL   tlb_slot
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; MMU on, then the L1s
    LLI  R5, #1
    WRSYS R5, #MMU, #MMUCR
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL

    LLI  R7, #0
    LLI  R6, #BODY_PA
    JMP  R6

; ── Copied to page 1; position-independent ──────────────────────
body:
    ; One instruction behind its producer
    LLI   R2, #0x11
    WRSYS R2, #MMU, #TLB_INDEX
    RDSYS R3, #MMU, #TLB_INDEX
    CMP   R3, #0x11
    BNE   b_fail

    ; Two behind
    LLI   R2, #0x22
    LLI   R4, #0
    WRSYS R2, #MMU, #TLB_INDEX
    RDSYS R3, #MMU, #TLB_INDEX
    CMP   R3, #0x22
    BNE   b_fail

    ; Three behind
    LLI   R2, #0x33
    LLI   R4, #0
    LLI   R4, #0
    WRSYS R2, #MMU, #TLB_INDEX
    RDSYS R3, #MMU, #TLB_INDEX
    CMP   R3, #0x33
    BNE   b_fail

    ; One behind an ALU result: 0x33 + 0x11
    ADD   R2, #0x11
    WRSYS R2, #MMU, #TLB_INDEX
    RDSYS R3, #MMU, #TLB_INDEX
    CMP   R3, #0x44
    BNE   b_fail

    ADD   R7, #1
    CMP   R7, #2
    BNE   body
    LLI   R1, #1
    BREAK
b_fail:
    LLI   R1, #0
    BREAK
body_end:

.include "mmu.inc"
