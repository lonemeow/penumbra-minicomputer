; cache_data.s -- data accesses whose cache outcomes are known per generation.
;
; The code runs from an uncached page, so only the loads and stores below
; reach the modelled caches.  run_iss_tests.py derives each generation's
; counts from these phases:
;
;   A  one load from each of 128 consecutive lines (2 KiB at 0x1000), cold.
;   B  the same 128 loads again.  A 1 KiB direct-mapped L1 has lost the
;      first half to the second by then; a 4 KiB 2-way L1 holds them all.
;   C  a store to 0x1000 and one to 0x3000, a line never loaded.
;   D  L1-D INVAL_ALL, then a load from 0x1000: L1 misses, L2 serves it.
;   E  eight loads over five lines 16 KiB apart -- one L2 set -- each after
;      an L1-D INVAL_ALL so every one reaches L2: A0 A1 A2 A3 A1 A0 A4 A2.
;      The L2's tree-PLRU sets each node on a used way's path to point
;      away from it and never prefers an invalid way: after the two hits
;      it points at A3, so A4 evicts A3 and A2 still hits -- 5 misses.
;      LRU, tree-PLRU taking invalid ways first, and a tree that toggles
;      its nodes instead of setting them all evict A2: 6.

.equ ROM_PTE,   0xFFFF00A9     ; V|R|X|G, uncached
.equ DATA_PTE,  0x9D           ; V|C|R|W|G

_start:
    ; Pinned slot 0: the ROM page, uncached, so fetches stay out of the caches
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE

    ; Identity-mapped cacheable data pages, each in its own TLB set
    LLI  R9, #DATA_PTE
    LLI  R3, #0x01
    BL   map_page
    LLI  R3, #0x03
    BL   map_page
    LLI  R3, #0x20
    BL   map_page
    LLI  R3, #0x24
    BL   map_page
    LLI  R3, #0x28
    BL   map_page
    LLI  R3, #0x2C
    BL   map_page
    LLI  R3, #0x30
    BL   map_page

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    ; A
    LI   R5, #0x1000
    LLI  R6, #128
phase_a:
    LDW  R7, [R5]
    ADD  R5, #16
    SUB  R6, #1
    BNE  phase_a

    ; B
    LI   R5, #0x1000
    LLI  R6, #128
phase_b:
    LDW  R7, [R5]
    ADD  R5, #16
    SUB  R6, #1
    BNE  phase_b

    ; C
    LI   R5, #0x1000
    STW  R7, [R5]
    LI   R5, #0x3000
    STW  R7, [R5]

    ; D
    LI   R5, #0x1000
    BL   inval_load

    ; E
    LI   R5, #0x20000
    BL   inval_load
    LI   R5, #0x24000
    BL   inval_load
    LI   R5, #0x28000
    BL   inval_load
    LI   R5, #0x2C000
    BL   inval_load
    LI   R5, #0x24000
    BL   inval_load
    LI   R5, #0x20000
    BL   inval_load
    LI   R5, #0x30000
    BL   inval_load
    LI   R5, #0x28000
    BL   inval_load
    BREAK

; Map VPN R3 to the same PPN in main-TLB set VPN[4:0], way 0, flags R9.
map_page:
    MOV  R2, R3
    AND  R2, #0x1F
    WRSYS R2, #MMU, #TLB_INDEX
    MOV  R2, R3
    SHL  R2, #8
    WRSYS R2, #MMU, #TLB_VPN
    MOV  R2, R3
    SHL  R2, #12
    OR   R2, R9
    WRSYS R2, #MMU, #TLB_PTE
    JMP  R13

; Drop every L1-D line, then load from R5.
inval_load:
    WRSYS R4, #DCACHE, #CACHE_INVAL_ALL
    LDW  R7, [R5]
    JMP  R13
