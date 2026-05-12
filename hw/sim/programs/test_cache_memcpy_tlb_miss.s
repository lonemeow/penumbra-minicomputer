; test_cache_memcpy_tlb_miss.s — cached memcpy with kernel TLB-miss
;                                 handler invocation during the burst.
;
; NetBSD's demand paging works by leaving user pages without TLB
; entries until first touch; on TLB miss the kernel handler walks the
; in-memory page tables, installs the entry, and ERETs back.  This
; test reproduces the cache+arbiter+adapter view of that pattern at a
; minimum: the destination page is left unmapped, the first STW to
; it takes a TLB_MISS, the handler installs the entry, and the loop
; continues.  The handler also runs in cached supervisor mode, so
; while it's running it generates its own cache fills on whatever
; instructions and data it touches — that's the kernel-handler
; interleave the unit testbench can't model.
;
; Result: R1 = 1 PASS, R1 = 0 FAIL (data mismatch or handler never
; fired, which would mean the test didn't exercise the intended
; path).

.equ N_WORDS,    1024
.equ SRC_PA,     0x1000
.equ DST_PA,     0x2000
.equ SRC_VA,     0x1000
.equ DST_VA,     0x2000
.equ KERN_RWX_C, 0xBD

_start:
    LLI  R1, #0
    LLI  R11, #0                 ; TLB-miss handler invocation counter

    ; ── Install TLB-miss handler at vector 2 ────────────────
    LA   R2, #tlb_miss_handler
    STW  R2, [R0 + #8]           ; vector[2] = TLB_MISS

    ; ── Phase 1: plant pattern at SRC_PA (MMU off) ──────────
    LLI  R5, #SRC_PA
    LLI  R6, #0
plant_loop:
    LUI  R7, #0x8000
    OR   R7, R6
    STW  R7, [R5]
    ADD  R5, #4
    ADD  R6, #1
    CMP  R6, #N_WORDS
    BNE  plant_loop

    ; ── Phase 2: set up TLB *without* a destination entry ───
    ; Slot 0: VPN 0 (kernel data scratch)
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 1: VPN 1 (source) — installed.
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10BD
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 16: ROM page (so handler+main fetch keep working).
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; NOTE: VPN 2 (DST) deliberately *not* installed.  The first
    ; STW to DST_VA inside the memcpy will fault, the handler
    ; will install the entry, and the burst will resume.

    ; ── Enable MMU + D-cache ────────────────────────────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR
    WRSYS R6, #DCACHE, #CACHE_CTRL

    ; ── Phase 3: memcpy.  First STW takes a TLB miss. ───────
    LLI  R5, #SRC_VA
    LLI  R6, #DST_VA
    LLI  R8, #0
memcpy_loop:
    LDW  R7, [R5]
    STW  R7, [R6]
    ADD  R5, #4
    ADD  R6, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  memcpy_loop

    ; ── Phase 4: verify dst against expected pattern ────────
    LLI  R5, #DST_VA
    LLI  R8, #0
verify_loop:
    LDW  R7, [R5]
    LUI  R9, #0x8000
    OR   R9, R8
    CMP  R7, R9
    BNE  fail
    ADD  R5, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  verify_loop

    ; Sanity: the TLB miss handler must have fired at least once.
    CMP  R11, R0
    BEQ  fail

    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; TLB miss handler — install VPN 2 → PPN 2 in TLB slot 2, ERET.
;
; Touches R10..R12 (TLB sysreg ops) and R11 (counter); all unused
; by the memcpy loop, so loop state survives the trap intact.
; The handler does its own bus traffic (the WRSYS sysreg ops are
; CPU-internal, but instruction fetches go through the I-cache
; and the ROM-page mapping), which is the kernel-interleave the
; test exists to exercise.
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    ADD   R11, #1
    LLI   R12, #2
    WRSYS R12, #MMU, #TLB_INDEX
    LLI   R12, #0x0200            ; VPN 2, ASID 0
    WRSYS R12, #MMU, #TLB_VPN
    LLI   R12, #0x20BD            ; PPN 2, V|C|R|W|X|G
    WRSYS R12, #MMU, #TLB_PTE
    ERET
