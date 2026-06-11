; test_tlb_remap.s — TLB non-identity mapping: prove address translation
; REQUIRES: mmu
;
; Proves the TLB actually remaps addresses by reading through a virtual
; address that maps to a DIFFERENT physical page than identity would.
;
; Key constraint: simple_mem is 16KB (0x0000–0x3FFF). Addresses above
; 0x3FFF alias (only bits [13:2] are used). So both sentinel values
; and the remap target must be within the 16KB physical range.
;
; Setup (MMU off):
;   - Store 0xDEAD at physical 0x1000 (page 1)
;   - Store 0xBEEF at physical 0x2000 (page 2, different physical page)
;   - Map VPN 0 → PPN 0 (code page, identity, X+R+G)
;   - Map VPN 2 → PPN 1 (remap: virtual 0x2000 → physical 0x1000)
;
; Test:
;   - Read from virtual 0x2000 → should get 0xDEAD (from physical 0x1000)
;   - Identity would give 0xBEEF (from physical 0x2000)
;
; Result: R1=1 PASS, R1=0 FAIL
;
; TLB entry format:
;   VPN word = {4'b0, VPN[19:0], ASID[7:0]}
;   PTE word = {PPN[19:0], SW[3:0], flags[7:0]}
;   Flags: V=bit0, C=bit2, R=bit3, W=bit4, X=bit5, U=bit6, G=bit7
;
; MMU sysreg addresses (device 0):
;   0=MMUCR  3=TLB_VPN  4=TLB_PTE  5=TLB_INDEX

_start:
    LLI  R1, #0               ; assume fail

    ; ── Plant sentinel values (MMU off = bypass mode) ───────
    ; Physical page 1 (0x1000): the value we expect via remap
    LLI  R10, #0xDEAD
    LLI  R3, #0x1000
    STW  R10, [R3]             ; mem[0x1000] = 0xDEAD

    ; Physical page 2 (0x2000): the decoy (identity would read this)
    LLI  R11, #0xBEEF
    LLI  R3, #0x2000
    STW  R11, [R3]             ; mem[0x2000] = 0xBEEF

    ; ── Map VPN 0 → PPN 0 for code execution (identity) ────
    ; TLB set 0, way 0
    LLI  R4, #0
    WRSYS R4, #0, #5           ; TLB_INDEX = 0
    WRSYS R4, #0, #3           ; TLB_VPN = 0 (VPN=0, ASID=0)
    LLI  R5, #0x00A9           ; PTE: PPN=0, flags = V|R|X|G = 0xA9
    WRSYS R5, #0, #4           ; commits code page entry

    ; ── Map VPN 2 → PPN 1 (remap: VA 0x2000 → PA 0x1000) ──
    ; TLB set 2 (VPN[4:0]=2), way 0
    LLI  R4, #2
    WRSYS R4, #0, #5           ; TLB_INDEX = 2
    LLI  R4, #0x0200           ; VPN word: VPN=2, ASID=0
    WRSYS R4, #0, #3           ; TLB_VPN (staged)
    LLI  R4, #0x1089           ; PTE: PPN=1 (bits[31:12]=0x00001), flags=V|R|G=0x89
    WRSYS R4, #0, #4           ; TLB_PTE (commits entry)

    ; ── Map ROM page (VPN 0xFFFF0 → PPN 0xFFFF0) ──────────
    LLI  R4, #16
    WRSYS R4, #0, #5           ; TLB_INDEX = 30
    LI   R4, #0x0FFFF000       ; VPN=0xFFFF0, ASID=0
    WRSYS R4, #0, #3
    LI   R4, #0xFFFF00B9       ; PPN=0xFFFF0, flags=V|R|W|X|G
    WRSYS R4, #0, #4

    ; ── Enable MMU ──────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #0, #0           ; MMUCR = 1 → MMU on

    ; ── Read through remapped address ───────────────────────
    ; VA 0x2000 → TLB → PA 0x1000 → should get 0xDEAD
    ; (identity would give PA 0x2000 → 0xBEEF)
    LLI  R5, #0x2000
    LDW  R6, [R5]
    CMP  R6, R10               ; R10 = 0xDEAD
    BNE  fail

    ; ── Disable MMU ─────────────────────────────────────────
    LLI  R4, #0
    WRSYS R4, #0, #0           ; MMUCR = 0 → bypass mode

    ; ── All checks passed ───────────────────────────────────
    LLI  R1, #1                ; PASS
fail:
    BREAK
