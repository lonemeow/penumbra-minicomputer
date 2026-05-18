; test_tlb_cow_inv.s — Copy-on-write with explicit D-cache invalidate
;
; Why a second variant?
; ─────────────────────
; test_tlb_cow.s leaves the D-cache untouched after the COW handler
; remaps VPN 1 → PPN 2.  The stale cache line at index[VPN1] then has
; tag=PPN1 while the new translation produces tag=PPN2 — a tag-mismatch
; miss is *what should* drive the post-retry refill from the new
; physical page.  That assumes the tag check is correct.
;
; This variant adds a tactical `WRSYS R0, #DCACHE, #CACHE_INVAL_ALL` at the
; tail of the handler.  Now the cache line is gone outright; the
; post-retry STW and the verifying LDW both take *cold* misses and
; refill from PPN 2 via the bus arbiter.  Two independent guarantees
; that the new physical page is what's read back:
;
;   1. The tactical invalidate ensures we cannot accidentally read a
;      cached PPN 1 line via a buggy tag-check.
;   2. The post-retry refill is a *cold* fill rather than a tag-mismatch
;      fill — a slightly different timing footprint through the bus
;      arbiter and SDRAM.
;
; If both tests pass, COW correctness is well-established.  If
; test_tlb_cow.s fails but this one passes, a regression is in the
; tag-mismatch refill path; if both fail, in COW logic.
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
.equ NEW_VAL,    0xBEEF

_start:
    LA   R2, #prot_handler
    STW  R2, [R0 + #0x0C]
    B    start

; ═══════════════════════════════════════════════════════════════
; Protection fault handler — COW + invalidate
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

    ; Remap VPN 1 → PPN 2 with W set
    LLI   R8, #1
    WRSYS R8, #MMU, #TLB_INDEX
    LLI   R8, #0x0100
    WRSYS R8, #MMU, #TLB_VPN
    LLI   R8, #0x20BD            ; PPN=2, V|C|R|W|X|G
    WRSYS R8, #MMU, #TLB_PTE

    ; ── The differentiator: tactical D-cache invalidate ───────
    ; After this, the cache holds no lines at all; the retried STW
    ; and the verifying LDW both cold-miss and refill from PPN 2
    ; via the bus arbiter.
    WRSYS R0, #DCACHE, #CACHE_INVAL_ALL

    ERET

; ═══════════════════════════════════════════════════════════════
; Main test (identical to test_tlb_cow.s up to the handler return)
; ═══════════════════════════════════════════════════════════════
start:
    LLI  R1, #0

    ; Plant sentinels in PPN 1 (MMU off)
    LI   R2, #SENT_W0
    LI   R3, #COW_VADDR
    STW  R2, [R3]
    LI   R2, #SENT_W1
    STW  R2, [R3 + #4]
    LI   R2, #SENT_W2
    STW  R2, [R3 + #8]
    LI   R2, #SENT_W3
    STW  R2, [R3 + #12]

    ; Slot 0: VPN 0 → PPN 0 RWX cacheable (kernel/handler)
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 1: VPN 1 → PPN 1 R-only cacheable (the COW page)
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10AD             ; PPN=1, V|C|R|X|G  (NO W)
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 2: VPN 2 → PPN 2 RWX cacheable (COW destination)
    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x20BD
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 16: ROM page
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; Enable MMU + D-cache
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR
    WRSYS R6, #DCACHE, #CACHE_CTRL

    ; Prime D-cache with a read of the COW page
    LI   R5, #COW_VADDR
    LDW  R2, [R5]
    LI   R3, #SENT_W0
    CMP  R2, R3
    BNE  fail

    ; Trigger the COW fault
    LLI  R10, #COW_VADDR
    LLI  R11, #FSTAT_W
    LI   R7, #NEW_VAL
    STW  R7, [R5]                ; faults → handler does COW + INVAL → ERET → retry

    ; After ERET the cache is empty.  Both of these LDWs cold-miss
    ; and refill from PPN 2 through the bus arbiter.
    LDW  R2, [R5]
    LI   R3, #NEW_VAL
    CMP  R2, R3
    BNE  fail

    LDW  R2, [R5 + #4]
    LI   R3, #SENT_W1
    CMP  R2, R3
    BNE  fail

    ; Drop MMU + D-cache, verify physical pages directly
    LLI  R6, #0
    WRSYS R6, #DCACHE, #CACHE_CTRL
    WRSYS R6, #MMU, #MMUCR

    ; PPN 1 must still hold the original sentinel (COW invariant)
    LI   R5, #COW_VADDR
    LDW  R2, [R5]
    LI   R3, #SENT_W0
    CMP  R2, R3
    BNE  fail

    LDW  R2, [R5 + #4]
    LI   R3, #SENT_W1
    CMP  R2, R3
    BNE  fail

    ; PPN 2 must hold the new value at offset 0 and the copied
    ; sentinel at offset 4
    LI   R5, #COW_DST_VA
    LDW  R2, [R5]
    LI   R3, #NEW_VAL
    CMP  R2, R3
    BNE  fail

    LDW  R2, [R5 + #4]
    LI   R3, #SENT_W1
    CMP  R2, R3
    BNE  fail

    LLI  R1, #1
fail:
    BREAK
