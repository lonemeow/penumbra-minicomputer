; test_trap_store.s — a cached memory store inside a fault handler
; REQUIRES: mmu cache l2
;
; The boot ROM's unhandled_trap stores to its (cached) trap stack on
; entry; gen2 Dhrystone wedges on that very store with I/D/L2 caches
; enabled.  No existing conformance test does a *memory store* inside a
; fault handler — test_cache_memcpy_tlb_miss's handler only issues
; CPU-internal WRSYS sysreg ops — so the post-fault cached write-through
; path is unexercised.  This reproduces it at a minimum: enable I+D+L2,
; take a TLB miss, and have the handler do a cached STW to a cold high
; SDRAM line (the trap-stack address class) followed by a read-back.
;
; If the post-fault store deadlocks, the runner hits its cycle cap
; (timeout / FAIL).  Result: R1 = 1 PASS, R1 = 0 FAIL.

.equ TRIG_VA,    0x2000        ; deliberately left unmapped → first touch faults
.equ HI_VA,      0x00FFF000    ; cold high SDRAM page (trap-stack address class)
.equ SENTINEL,   0x5A5A1234
.equ KERN_RWX_C, 0xBD          ; V|C|R|W|X|G — cached kernel page

_start:
    LLI  R1, #0                  ; assume fail
    LLI  R11, #0                 ; handler-fired flag

    ; ── vector[2] = TLB-miss handler ───────────────────────
    LA   R2, #tlb_miss_handler
    STW  R2, [R0 + #8]

    ; ── TLB slot 0: page 0 (cached vectors + scratch) ──────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    ; ── TLB slot for high page 0x00FFF000 (cached) ─────────
    ; Main TLB is set-associative: index = (VPN & 0x1F) | way<<5.
    ; VPN 0xFFF → set 0x1F, way 0.
    LLI  R2, #0x1F
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x000FFF00          ; VPN = HI_VA >> 4
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0x00FFF0BD          ; PA HI_VA | cached
    WRSYS R3, #MMU, #TLB_PTE

    ; ── TLB slot 16: ROM page (handler + main keep fetching) ─
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; NOTE: TRIG_VA (VPN 2) deliberately *not* installed — the LDW
    ; below faults, the handler installs it and does the test store.

    ; ── Enable MMU + all caches (match Dhrystone crt0) ─────
    LLI  R6, #1
    WRSYS R6, #MMU,    #MMUCR
    WRSYS R6, #DCACHE, #CACHE_CTRL
    WRSYS R6, #ICACHE, #CACHE_CTRL
    WRSYS R6, #L2,     #CACHE_CTRL

    ; ── Trigger the fault ──────────────────────────────────
    LI   R5, #TRIG_VA
    LDW  R7, [R5]                 ; TLB miss → handler runs (and stores)

    ; ── Verify the handler's post-fault cached store landed ─
    CMP  R11, R0                  ; handler must have fired
    BEQ  fail
    LI   R5, #HI_VA
    LDW  R7, [R5]                 ; read back what the handler stored
    LI   R8, #SENTINEL
    CMP  R7, R8
    BNE  fail

    LLI  R1, #1                   ; PASS
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; TLB-miss handler — install the trigger page, then do the store
; under test: a cached write-through STW to a cold high SDRAM line,
; the analogue of the ROM trap handler's first stack write.  ERET.
; Uses R10..R12 only (main uses R1..R9), so the faulting LDW's
; surrounding state is preserved across the trap.
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    LLI   R11, #1                 ; mark fired

    ; install TRIG_VA (VPN 2 → PA 0x2000) so the faulting LDW resumes
    LLI   R12, #2
    WRSYS R12, #MMU, #TLB_INDEX
    LLI   R12, #0x0200            ; VPN 2, ASID 0
    WRSYS R12, #MMU, #TLB_VPN
    LLI   R12, #0x20BD            ; PA 0x2000 | V|C|R|W|X|G
    WRSYS R12, #MMU, #TLB_PTE

    ; the store under test — cached write-through to the cold high line
    LI    R10, #HI_VA
    LI    R12, #SENTINEL
    STW   R12, [R10]              ; <-- Dhrystone wedges on the analogous store

    ERET
