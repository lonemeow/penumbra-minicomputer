; test_tlb_cow.s — Copy-on-write semantics on a CACHEABLE page
;
; Why this test exists
; ────────────────────
; The existing test_tlb_prot.s exercises protection faults on
; *uncacheable* pages (PTE.C=0) and only "remaps with W" — it does
; not actually copy.  That misses two things:
;
;   1. The cached prot-fault path (PTE.C=1, MMU on, D-cache on) —
;      this is the path that NetBSD userspace fork()/COW takes.
;   2. The COW invariant itself: after a write fault triggers a copy,
;      the *original* physical page must remain untouched, because
;      other processes are still sharing it.  A handler that only
;      remaps with W (no copy) silently breaks every other sharer.
;
; What this test does
; ───────────────────
;   • Plants a sentinel pattern at physical page 1 (0x1000…0x100F).
;   • Maps VPN 1 → PPN 1 R-only and CACHEABLE (no W).
;   • Maps VPN 2 → PPN 2 RWX cacheable — the COW destination.
;   • Maps VPN 0 → PPN 0 RWX cacheable — kernel/handler.
;   • Enables MMU + D-cache, primes the cache with a load from VPN 1.
;   • Stores to VPN 1 → expects VEC_TLB_PROT.
;   • Handler does the COW: copies PPN 1 → PPN 2, remaps VPN 1 → PPN 2
;     with W, ERETs to retry the store.
;   • Verifies the retried store landed in PPN 2 (new page).
;   • Verifies PPN 1 is *unchanged* (original sentinel intact) —
;     this is the COW invariant.
;
; Register convention (mirrors test_tlb_prot.s where possible)
;   R1        = pass/fail result
;   R2-R6     = setup scratch
;   R7        = data being stored (must survive the fault)
;   R8,R9,R13 = handler scratch
;   R10       = expected FAULT_ADDR
;   R11       = expected FSTAT bit (FSTAT_W)
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX_C, 0xBD       ; V|C|R|W|X|G — cacheable kernel page
.equ KERN_RX_C,  0xAD       ; V|C|R|X|G   — cacheable, READ-ONLY
.equ FAULT_PROT, 2          ; FAULT_STATUS[3:0]
.equ FSTAT_W,    0x200      ; bit [9] = write access

.equ COW_VADDR,  0x1000     ; in VPN 1 — the COW page
.equ COW_DST_VA, 0x2000     ; in VPN 2 — destination mapping
.equ SENT_W0,    0xCAFE0001
.equ SENT_W1,    0xCAFE0002
.equ SENT_W2,    0xCAFE0003
.equ SENT_W3,    0xCAFE0004
.equ NEW_VAL,    0xBEEF     ; value the faulting STW writes

; ═══════════════════════════════════════════════════════════════
; Entry point — install handler, jump to setup
; ═══════════════════════════════════════════════════════════════
_start:
    LA   R2, #prot_handler
    STW  R2, [R0 + #0x0C]      ; VEC_TLB_PROT vector slot
    B    start

; ═══════════════════════════════════════════════════════════════
; Protection fault handler — does the COW work
;
; On entry (set by main program):
;   R10 = expected FAULT_ADDR
;   R11 = expected FSTAT bit (FSTAT_W here)
;
; Pre-checks (verify we got the fault we expected) are filled in;
; the actual COW step is left to you.
; ═══════════════════════════════════════════════════════════════
prot_handler:
    ; Verify FAULT_ADDR matches expected
    RDSYS R8, #MMU, #FAULT_ADDR
    CMP   R8, R10
    BNE   fail

    ; Verify fault type = FAULT_PROT
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F
    MOV   R13, R8
    AND   R13, R9
    CMP   R13, #FAULT_PROT
    BNE   fail

    ; Verify expected access bit (W)
    MOV   R13, R8
    AND   R13, R11
    CMP   R13, R11
    BNE   fail

    ; Copy the first 4 words of source page to destination page
    LI  R9,  #COW_VADDR
    LI  R8,  #COW_DST_VA
    LDW R13, [R9]
    STW R13, [R8]
    LDW R13, [R9 + #4]
    STW R13, [R8 + #4]
    LDW R13, [R9 + #8]
    STW R13, [R8 + #8]
    LDW R13, [R9 + #12]
    STW R13, [R8 + #12]

    ; ── Slot 1: VPN 1 → PPN 2, RWX cacheable ─
    LLI   R8, #1
    WRSYS R8, #MMU, #TLB_INDEX
    LLI   R8, #0x0100            ; VPN=2, ASID=0
    WRSYS R8, #MMU, #TLB_VPN
    LLI   R8, #0x20BD            ; PPN=2, V|C|R|W|X|G
    WRSYS R8, #MMU, #TLB_PTE

    ; Return from trap
    ERET

; ═══════════════════════════════════════════════════════════════
; Main test
; ═══════════════════════════════════════════════════════════════
start:
    LLI  R1, #0                 ; assume fail until proven otherwise

    ; ── Plant sentinel words in PPN 1 (with MMU off) ──────────
    LI   R2, #SENT_W0
    LI   R3, #COW_VADDR         ; physical 0x1000 while MMU off
    STW  R2, [R3]
    LI   R2, #SENT_W1
    STW  R2, [R3 + #4]
    LI   R2, #SENT_W2
    STW  R2, [R3 + #8]
    LI   R2, #SENT_W3
    STW  R2, [R3 + #12]

    ; ── Slot 0: VPN 0 → PPN 0, kernel RWX cacheable ───────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Slot 1: VPN 1 → PPN 1, READ-ONLY cacheable (the COW page)
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100            ; VPN=1, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10AD            ; PPN=1, V|C|R|X|G  (NO W)
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Slot 2: VPN 2 → PPN 2, RWX cacheable (COW destination) ─
    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200            ; VPN=2, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x20BD            ; PPN=2, V|C|R|W|X|G
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Slot 16: ROM page (so we can keep fetching) ───────────
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU + D-cache ──────────────────────────────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR
    WRSYS R6, #DCACHE, #CACHE_CTRL

    ; ── Prime D-cache with a read of the COW page ─────────────
    ; This is the bit kernel-mode tests don't do today: a cached
    ; load on a page that's about to take a W-fault.
    LI   R5, #COW_VADDR
    LDW  R2, [R5]
    LI   R3, #SENT_W0
    CMP  R2, R3
    BNE  fail

    ; ── Trigger the COW fault ─────────────────────────────────
    LLI  R10, #COW_VADDR        ; expected FAULT_ADDR
    LLI  R11, #FSTAT_W          ; expected fault access bit
    LI   R7, #NEW_VAL
    STW  R7, [R5]               ; faults → handler does COW → ERET → retry

    ; ── Verify: cached read from VPN 1 now sees the new page ──
    LDW  R2, [R5]
    LI   R3, #NEW_VAL
    CMP  R2, R3
    BNE  fail

    ; Word 1 of VPN 1 should be SENT_W1 (copied from PPN 1)
    LDW  R2, [R5 + #4]
    LI   R3, #SENT_W1
    CMP  R2, R3
    BNE  fail

    ; ── Drop MMU + D-cache, verify physical pages directly ────
    LLI  R6, #0
    WRSYS R6, #DCACHE, #CACHE_CTRL
    WRSYS R6, #MMU, #MMUCR
    ; D-cache is write-through, so RAM is up to date. But we just
    ; bypassed the cache anyway, so what we read is real DRAM.

    ; PPN 1 (physical 0x1000) MUST still hold the original sentinel.
    ; If this fails, the handler clobbered the source page — i.e.
    ; it remapped without copying, or it copied the wrong direction.
    LI   R5, #COW_VADDR
    LDW  R2, [R5]
    LI   R3, #SENT_W0
    CMP  R2, R3
    BNE  fail                    ; COW invariant violated

    LDW  R2, [R5 + #4]
    LI   R3, #SENT_W1
    CMP  R2, R3
    BNE  fail

    ; PPN 2 (physical 0x2000) must hold the new value at offset 0
    ; and the copied sentinel at offset 4.
    LI   R5, #COW_DST_VA
    LDW  R2, [R5]
    LI   R3, #NEW_VAL
    CMP  R2, R3
    BNE  fail

    LDW  R2, [R5 + #4]
    LI   R3, #SENT_W1
    CMP  R2, R3
    BNE  fail

    ; ── All checks passed ─────────────────────────────────────
    LLI  R1, #1
fail:
    BREAK
