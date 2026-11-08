; test_tlb_refill_cached.s — A TLB refill by a miss handler that runs from
; the caches takes effect
; REQUIRES: mmu-d mmu-i cache l2
;
; The shape a loaded program has once it has brought the MMU and the caches
; up: the miss handler runs from a cacheable RAM page through a pinned
; slot, and the missing page is a high virtual page in the same main-TLB
; set as the handler's own page.
;   1. Copy the handler to RAM page 1 (0x1000) and the target code to
;      page 2 (0x2000)
;   2. Pin page 0 (vectors) and page 1 (handler, scratch), both cacheable;
;      map the ROM page; leave TARGET_VA unmapped
;   3. Enable the MMU, then the L1-D, the L1-I and the L2
;   4. JMP to TARGET_VA (0x80001000, main-TLB set 1, as page 1 is) →
;      instruction fetch TLB miss
;   5. The handler maps TARGET_VA to page 2, cacheable, and ERETs
;   6. Retried fetch succeeds, planted code sets R1=1 and BREAKs
;
; A retried fetch that misses the TLB again re-enters the handler, which
; FAILs on its second entry.
;
; Register convention:
;   R1  = pass/fail result
;   R2-R6 = setup scratch
;   R8,R9 = handler scratch
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWXC,   0xBD          ; V|C|R|W|X|G — kernel full access, cacheable
.equ HANDLER_PA,  0x1000        ; page 1: handler code + scratch
.equ COUNT,       0x1F00        ; handler entry count, in page 1
.equ TARGET_PA,   0x2000        ; page 2: the target code
.equ TARGET_VA,   0x80001000    ; VPN 0x80001: set 1, unmapped at the jump

_start:
    LLI  R1, #0                 ; assume fail
    STW  R0, [R0 + #COUNT]

    ; vector[2] = the handler's copy
    LLI  R2, #HANDLER_PA
    STW  R2, [R0 + #8]

    ; Copy the handler to page 1
    LA   R2, #handler_start
    LLI  R3, #HANDLER_PA
    LA   R4, #handler_end
copy_handler:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy_handler

    ; Copy the target code to page 2
    LA   R2, #target_code
    LLI  R3, #TARGET_PA
    LA   R4, #target_code_end
copy_target:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy_target

    ; Pinned slot 0: page 1 identity, cacheable
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0100            ; VPN = 1, ASID = 0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #0x10BD            ; PPN = 1, KERN_RWXC
    WRSYS R2, #MMU, #TLB_PTE

    ; Pinned slot 1: page 0 identity, cacheable
    LLI  R2, #0x41
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #KERN_RWXC
    WRSYS R2, #MMU, #TLB_PTE

    ; Main TLB slot 16: the ROM page identity, uncacheable as in the other
    ; MMU tests
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; MMU on, then the caches
    LLI  R5, #1
    WRSYS R5, #MMU, #MMUCR
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    ; Jump to the unmapped target
    LI   R6, #TARGET_VA
    JMP  R6

    ; Not reached: the jump faults, and the retry lands in the target
    BREAK

; ── Copied to page 1; position-independent ──────────────────────
handler_start:
    ; A second entry is the retried fetch missing again
    LDW   R9, [R0 + #COUNT]
    ADD   R9, #1
    STW   R9, [R0 + #COUNT]
    CMP   R9, #1
    BNE   h_fail

    RDSYS R8, #MMU, #FAULT_ADDR
    LI    R9, #TARGET_VA
    CMP   R8, R9
    BNE   h_fail

    ; Main slot 1 (set 1, way 0): TARGET_VA → page 2, cacheable
    LLI   R9, #1
    WRSYS R9, #MMU, #TLB_INDEX
    LI    R9, #0x08000100       ; VPN = 0x80001, ASID = 0
    WRSYS R9, #MMU, #TLB_VPN
    LI    R9, #0x20BD           ; PPN = 2, KERN_RWXC
    WRSYS R9, #MMU, #TLB_PTE
    ERET
h_fail:
    LLI   R1, #0
    BREAK
handler_end:

; ── Copied to page 2 ─────────────────────────────────────────────
target_code:
    LLI  R1, #1
    BREAK
target_code_end:
