; test_tlb_identity.s — TLB identity mapping: read/write with MMU enabled
;
; Tests:
;   1. Identity-map page 0 (VPN 0 → PPN 0) with R+W+X+G permissions
;   2. Enable MMU (set MMUCR.M = 1)
;   3. Store a value at 0x0800, load it back — both go through TLB
;   4. Disable MMU, read 0x0800 in bypass mode — verify store landed
;
; This proves the TLB translates both fetch (X) and data (R/W) accesses
; correctly when the mapping is VA == PA (identity).
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9       ; V|R|W|X|G — kernel code+data page

_start:
    LLI  R1, #0               ; assume fail

    ; ── Map VPN 0 → PPN 0 (slot 0) ────────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX  ; select slot 0
    WRSYS R2, #MMU, #TLB_VPN    ; VPN=0, ASID=0
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE    ; PPN=0, flags=V|R|W|X|G (commits)

    ; ── Enable MMU ──────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR      ; M=1 → MMU on

    ; ── From here, every fetch and data access goes through TLB ──

    ; Store test pattern 0xCAFE at address 0x0800 (within page 0)
    LLI  R5, #0xCAFE
    LLI  R6, #0x0800
    STW  R5, [R6]

    ; Load it back through TLB
    LDW  R7, [R6]
    CMP  R7, R5
    BNE  fail

    ; ── Disable MMU ─────────────────────────────────────────
    LLI  R4, #0
    WRSYS R4, #MMU, #MMUCR      ; M=0 → bypass mode

    ; Read physical 0x0800 directly (no TLB) — verify store landed
    LDW  R8, [R6]
    CMP  R8, R5
    BNE  fail

    ; ── All checks passed ───────────────────────────────────
    LLI  R1, #1                ; PASS
fail:
    BREAK
