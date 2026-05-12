; test_cache_memcpy.s — sustained cached LDW/STW memcpy through MMU
;
; Mimics the kernel-side bus traffic of pmap_copy_page: sequential
; reads from one cacheable page interleaved with sequential writes
; to another, all through TLB-translated addresses with the D-cache
; enabled.  This is the alleged trigger pattern for the residual
; NetBSD-userspace bug that the adapter unit tests don't cover
; because they have no cache or MMU in the loop.
;
; What this test does
; ───────────────────
;   • Phase 1 (MMU off): plant 64 words at physical 0x1000 with
;     pattern src[i] = 0x80000000 | i.
;   • Phase 2: set up TLB — VPN 1→PPN 1 cacheable, VPN 2→PPN 2
;     cacheable, plus VPN 0 + ROM page so fetch keeps working.
;   • Phase 3: enable MMU + D-cache, run the memcpy loop through
;     cached VAs (VPN 1 → VPN 2) for 64 words.
;   • Phase 4: read VPN 2 back word-by-word and verify each value
;     matches the expected pattern.
;
; If the residual bug is in this access pattern (cached memcpy
; through MMU at supervisor level), this test fails.  If it
; passes, the trigger needs more than this — user mode, IRQ
; delivery, TLB miss handler invocation, fork, or specific
; multi-process state.
;
; Result: R1 = 1 PASS, R1 = 0 FAIL

.equ N_WORDS,    1024         ; words to copy (4096 bytes = one page)
.equ SRC_PA,     0x1000       ; physical source page (PPN 1)
.equ DST_PA,     0x2000       ; physical destination page (PPN 2)
.equ SRC_VA,     0x1000       ; virtual source (VPN 1 → PPN 1)
.equ DST_VA,     0x2000       ; virtual dest (VPN 2 → PPN 2)
.equ KERN_RWX_C, 0xBD         ; V|C|R|W|X|G — cacheable RWX

_start:
    ; ── Phase 1: plant pattern at physical SRC_PA (MMU off) ──
    LLI  R5, #SRC_PA
    LLI  R6, #0                ; i
plant_loop:
    LUI  R7, #0x8000           ; R7 = 0x80000000
    OR   R7, R6                ; R7 = 0x80000000 | i
    STW  R7, [R5]
    ADD  R5, #4
    ADD  R6, #1
    CMP  R6, #N_WORDS
    BNE  plant_loop

    ; ── Phase 2: set up TLB ──────────────────────────────────
    ; Slot 0: VPN 0 → PPN 0 (kernel data scratch, cacheable RWX)
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 1: VPN 1 → PPN 1 cacheable RWX (source)
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100           ; VPN=1, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10BD           ; PPN=1, V|C|R|W|X|G
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 2: VPN 2 → PPN 2 cacheable RWX (destination)
    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200           ; VPN=2, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x20BD           ; PPN=2, V|C|R|W|X|G
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 16: ROM page (so fetch keeps working with MMU on)
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Phase 3: enable MMU + D-cache, run memcpy ────────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR
    WRSYS R6, #DCACHE, #CACHE_CTRL

    LLI  R5, #SRC_VA           ; src VA
    LLI  R6, #DST_VA           ; dst VA
    LLI  R8, #0                ; i
memcpy_loop:
    LDW  R7, [R5]              ; cache fill on every 4th word
    STW  R7, [R6]              ; write-through to bus
    ADD  R5, #4
    ADD  R6, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  memcpy_loop

    ; ── Phase 4: verify dst against expected pattern ─────────
    LLI  R5, #DST_VA           ; dst VA
    LLI  R8, #0                ; i
verify_loop:
    LDW  R7, [R5]
    LUI  R9, #0x8000
    OR   R9, R8                ; expected = 0x80000000 | i
    CMP  R7, R9
    BNE  fail
    ADD  R5, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  verify_loop

    ; All checks passed
    LLI  R1, #1
    BREAK

fail:
    LLI  R1, #0
    BREAK
