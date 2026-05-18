; test_l2_minimal.s — smallest possible L2-on-the-data-path test
;
; Goals:
;   1. L2 sysreg INFO is non-zero (i.e. HAS_L2=1 build, L2 present).
;   2. Enabling L2 via WRSYS CACHE_CTRL=1 doesn't hang the bus.
;   3. A single cached LDW through L1→L2→memory returns the right
;      value (cold miss into L2, then cold miss into L1, line fill,
;      data returned).
;
; The MMU must be on for cacheability to take effect — in bypass
; mode mmu_cacheable=0 so L1 and L2 both pass through.  Identity-map
; page 0 cacheable, ROM page uncacheable.
;
; This test must also pass with HAS_L2=0 — INFO reads 0, the enable
; write is skipped, but L1+MMU+memcpy of one value still works.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX_C, 0xBD     ; V|C|R|W|X|G — cacheable kernel page
.equ TEST_ADDR,  0x0800   ; test address within page 0
.equ TEST_VAL,   0xCAFE

_start:
    LLI  R1, #0                ; assume fail

    ; ── Identity-map page 0 cacheable ─────────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN       ; VPN=0, ASID=0
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE       ; PPN=0, cacheable

    ; ROM page uncacheable, so I-fetch never goes through caches
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00B9
    WRSYS R2, #MMU, #TLB_PTE

    ; ── Enable L2 if present (HAS_L2=1) ───────────────────────
    RDSYS R2, #L2, #CACHE_INFO
    CMP  R2, R0
    BEQ  no_l2                     ; HAS_L2=0 build: skip enable
    LLI  R4, #1
    WRSYS R4, #L2, #CACHE_CTRL
no_l2:

    ; ── Enable D-cache, then MMU ──────────────────────────────
    ; (Order: caches before MMU is fine — they only activate
    ; when mmu_cacheable=1, which requires MMU on + PTE.C=1.)
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #MMU, #MMUCR

    ; ── Single cached store + load ────────────────────────────
    ; STW is write-no-allocate at L1 (goes to memory bus); with
    ; L2 enabled, L2 sees the write — write-invalidate-on-hit
    ; means L2 drops any matching line and lets RAM take it.
    LLI  R5, #TEST_VAL
    LLI  R6, #TEST_ADDR
    STW  R5, [R6]

    ; First LDW: L1 cold miss → bus read.  With HAS_L2, the L2
    ; sees the request first — cold miss into L2 too, line fill
    ; from RAM, return to L1.  L1 latches the word and replays.
    LDW  R7, [R6]
    CMP  R7, R5
    BNE  fail

    ; ── PASS ──────────────────────────────────────────────────
    LLI  R1, #1
fail:
    BREAK
