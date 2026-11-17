; cache_aftermiss.s -- the instruction after each L1-D load miss.
;
; The code runs from an uncached page, so only the loads and stores below
; reach the modelled caches.  Every load in A-F is to a line not loaded
; before, except A's second.  The instruction after each miss is:
;
;   A  0x1000 misses; a load of 0x1004, the line just filled.
;   B  0x1040 misses; a load of 0x1440, which misses too.  Line 0x144 is
;      in set 4 of a 64-set L1 (0x1040's) and set 0x44 of a 256-set one,
;      so this is the filled set on penumbra1 and another on penumbra3.
;      0x1440's miss is followed by an ADD: no access.
;   C  0x1080 misses; a store to 0x1800, another set on both.
;   D  0x10C0 misses; an ADD, no access.
;   E  0x1100 misses; an uncached load of 0x3100, set 0x10 on both: an
;      access counts by its set whether or not it is cacheable.
;   F  0x1140 misses; a load of 0x1180, another set, which misses and is
;      followed by two ADDs: no access.
;
; Eight misses: on penumbra1 the filled set 3 (A B E), another set 2 (C F),
; no access 3 (B D F); on penumbra3 the filled set 2, another set 3.

.equ ROM_PTE,     0xFFFF00A9   ; V|R|X|G, uncached
.equ DATA_PTE,    0x9D         ; V|C|R|W|G
.equ UNCACHED_PTE, 0x99        ; V|R|W|G

_start:
    ; Pinned slot 0: the ROM page, uncached, so fetches stay out of the caches
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE

    ; Identity-mapped data pages: 0x1000 cacheable, 0x3000 not
    LLI  R9, #DATA_PTE
    LLI  R3, #0x01
    BL   map_page
    LLI  R9, #UNCACHED_PTE
    LLI  R3, #0x03
    BL   map_page

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    LI   R5, #0x1000
    LI   R6, #0x3000

    LDW  R7, [R5]              ; A
    LDW  R7, [R5 + #4]
    LDW  R7, [R5 + #0x40]      ; B
    LDW  R7, [R5 + #0x440]
    ADD  R8, #1
    LDW  R7, [R5 + #0x80]      ; C
    STW  R7, [R5 + #0x800]
    LDW  R7, [R5 + #0xC0]      ; D
    ADD  R8, #1
    LDW  R7, [R5 + #0x100]     ; E
    LDW  R7, [R6 + #0x100]
    LDW  R7, [R5 + #0x140]     ; F
    LDW  R7, [R5 + #0x180]
    ADD  R8, #1
    ADD  R8, #1
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
