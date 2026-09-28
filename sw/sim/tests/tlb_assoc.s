; tlb_assoc.s -- main-TLB translations that tell TLB layouts apart.
;
; The same harness as tlb_basic.s: code in a pinned slot, data pages
; refilled into main-TLB way 0 by the miss handler, so the real TLB
; misses on every conflict in a set.  The expected counts are for LRU
; shadows (random ones would depend on their draws):
;
;   A  VPNs 0x100 0x120 0x140, twice round.  All three share set 0 of a
;      32-set TLB: 2 ways thrash, 6 misses; 4 ways, or 64 sets that put
;      0x120 apart, hold all three, 3 misses.  Real: 6.
;   B  VPN 0x105 under ASID 1, ASID 2, ASID 1: an entry matches only its
;      own ASID, and two ways keep both, 2 misses.  Real: 3.
;   C  VPN 0x107 under ASID 1 and ASID 2, 2 misses; software invalidates
;      the real slot, which holds ASID 2's entry; then ASID 1, a hit,
;      and ASID 2, a miss: an invalidation drops only its own ASID's
;      entry.  Real: 4.
;   D  global VPN 0x186 under ASID 1, a miss, and ASID 2, a hit; software
;      invalidates its real slot; ASID 2 again misses: dropping a global
;      entry drops it for every ASID.  Real: 2.
;
; 32 sets x 2 ways: 13 misses; the larger layouts: 10.  Real: 15.

.equ ROM_PTE,       0xFFFF00A9     ; V|R|X|G, uncached
.equ FLAGS,         0x19           ; V|R|W
.equ GLOBAL_FLAGS,  0x99           ; V|R|W|G
.equ MMUCR_ASID1,   0x0101
.equ MMUCR_ASID2,   0x0201

_start:
    LA   R2, #tlb_miss
    STW  R2, [R0 + #8]             ; vector 2, read physically

    ; Pinned slot 0: the ROM page
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R8, #FLAGS
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; A
    LLI  R6, #2
round:
    LI   R5, #0x100000
    LDW  R7, [R5]
    LI   R5, #0x120000
    LDW  R7, [R5]
    LI   R5, #0x140000
    LDW  R7, [R5]
    SUB  R6, #1
    BNE  round

    ; B
    LI   R5, #0x105000
    LI   R2, #MMUCR_ASID1
    LI   R3, #MMUCR_ASID2
    WRSYS R2, #MMU, #MMUCR
    LDW  R7, [R5]
    WRSYS R3, #MMU, #MMUCR
    LDW  R7, [R5]
    WRSYS R2, #MMU, #MMUCR
    LDW  R7, [R5]

    ; C
    LI   R5, #0x107000
    WRSYS R2, #MMU, #MMUCR
    LDW  R7, [R5]
    WRSYS R3, #MMU, #MMUCR
    LDW  R7, [R5]
    LLI  R9, #7
    BL   invalidate_way0
    WRSYS R2, #MMU, #MMUCR
    LDW  R7, [R5]
    WRSYS R3, #MMU, #MMUCR
    LDW  R7, [R5]

    ; D
    LLI  R8, #GLOBAL_FLAGS
    LI   R5, #0x186000
    WRSYS R2, #MMU, #MMUCR
    LDW  R7, [R5]
    WRSYS R3, #MMU, #MMUCR
    LDW  R7, [R5]
    LLI  R9, #6
    BL   invalidate_way0
    LDW  R7, [R5]
    BREAK

; Invalidate main-TLB set R9, way 0, clearing the staged VPN too.
invalidate_way0:
    WRSYS R9, #MMU, #TLB_INDEX
    WRSYS R0, #MMU, #TLB_VPN
    WRSYS R0, #MMU, #TLB_PTE
    JMP  R13

; Map the faulting page to itself in main-TLB set VPN[4:0], way 0, under
; the current ASID, with flags R8.
tlb_miss:
    RDSYS R10, #MMU, #FAULT_ADDR
    SHR  R10, #12
    MOV  R11, R10
    AND  R11, #0x1F
    WRSYS R11, #MMU, #TLB_INDEX
    RDSYS R11, #MMU, #MMUCR
    SHR  R11, #8
    AND  R11, #0xFF
    MOV  R12, R10
    SHL  R12, #8
    OR   R12, R11
    WRSYS R12, #MMU, #TLB_VPN
    MOV  R12, R10
    SHL  R12, #12
    OR   R12, R8
    WRSYS R12, #MMU, #TLB_PTE
    ERET
