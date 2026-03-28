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
;
; TLB entry format:
;   VPN word = {4'b0, VPN[19:0], ASID[7:0]}
;   PTE word = {PPN[19:0], SW[3:0], flags[7:0]}
;   Flags: V=bit0, C=bit2, R=bit3, W=bit4, X=bit5, U=bit6, G=bit7
;
; MMU sysreg addresses (device 0):
;   0=MMUCR  1=FADDR  2=FSTAT  3=TLB_VPN  4=TLB_PTE  5=TLB_INDEX

_start:
    LLI  R1, #0               ; assume fail

    ; ── Map VPN 0 → PPN 0 (set 0, way 0) ───────────────────
    ; TLB set index = VPN[4:0] = 0, way = 0 → TLB_INDEX = 0
    LLI  R2, #0
    WRSYS R2, #0, #5           ; TLB_INDEX = 0

    ; VPN word: VPN=0x00000, ASID=0x00 → 0x00000000
    WRSYS R2, #0, #3           ; TLB_VPN = 0 (staged)

    ; PTE word: PPN=0x00000, SW=0, flags = V|R|W|X|G = 0xB9
    ;   V=0x01, R=0x08, W=0x10, X=0x20, G=0x80 → 0xB9
    LLI  R3, #0x00B9
    WRSYS R3, #0, #4           ; TLB_PTE = 0x000000B9 (commits entry)

    ; ── Enable MMU ──────────────────────────────────────────
    ; MMUCR: bit 0 = M (enable translation)
    LLI  R4, #1
    WRSYS R4, #0, #0           ; MMUCR = 1 → MMU on

    ; ── From here, every fetch and data access goes through TLB ──

    ; Store test pattern 0xCAFE at address 0x0800 (within page 0)
    LLI  R5, #0xCAFE
    LLI  R6, #0x0800
    STW  R5, [R6]              ; TLB translates: VA 0x0800 → PA 0x0800

    ; Load it back through TLB
    LDW  R7, [R6]              ; TLB translates: VA 0x0800 → PA 0x0800
    CMP  R7, R5
    BNE  fail                  ; mismatch → TLB load broken

    ; ── Disable MMU ─────────────────────────────────────────
    LLI  R4, #0
    WRSYS R4, #0, #0           ; MMUCR = 0 → bypass mode

    ; Read physical 0x0800 directly (no TLB) — verify store landed
    LDW  R8, [R6]
    CMP  R8, R5
    BNE  fail                  ; store didn't reach physical memory

    ; ── All checks passed ───────────────────────────────────
    LLI  R1, #1                ; PASS
fail:
halt:
    B    halt
