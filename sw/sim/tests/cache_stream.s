; cache_stream.s -- the stream buffers beside L2.
;
; The code runs from an uncached page, so only the loads and stores below
; reach the modelled caches.  Every load is to a line not loaded before,
; so each is an L1-D miss and an L2 miss: a memory fill.  Lines are named
; by their number, address / 16, all in the page at 0x1000.
;
;   1  0x100 starts a stream in one buffer, prefetching nothing.
;   2  0x101 is the line after it: two consecutive fills, so the buffer
;      starts prefetching, 0x102.
;   3  0x102, 0x103 and 0x104 are each served by the buffer, which
;      prefetches the next line each time, ending with 0x105 held.
;   4  0x1FE starts a stream in the other buffer, which held nothing.
;   5  0x1FF is the line after it, but 0x200 starts another page: no
;      prefetch.
;   6  0x105 is served, and the first buffer prefetches 0x106.
;   7  A store to 0x106 drops that prefetch, unused.
;   8  0x106 goes to memory, but it is the line after the first stream's
;      latest fill, so that buffer starts prefetching again, 0x107.
;   9  0x180 starts a stream in the second buffer, the one whose stream
;      moved least recently (step 5 against step 8), which held nothing.
;  10  0x1A0 starts one in the first buffer, now the least recent,
;      giving up its prefetch of 0x107 unused.
;
; Eleven memory fills: 4 served, 6 prefetches, 2 of them unused.

.equ ROM_PTE,     0xFFFF00A9   ; V|R|X|G, uncached
.equ DATA_PTE,    0x9D         ; V|C|R|W|G

_start:
    ; Pinned slot 0: the ROM page, uncached, so fetches stay out of the caches
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE

    ; Identity-mapped data page 0x1000, cacheable
    LLI  R9, #DATA_PTE
    LLI  R3, #0x01
    BL   map_page

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    LI   R5, #0x1000
    LI   R6, #0x1F00
    LI   R10, #0x1800

    LDW  R7, [R5]              ; 1
    LDW  R7, [R5 + #0x10]      ; 2
    LDW  R7, [R5 + #0x20]      ; 3
    LDW  R7, [R5 + #0x30]
    LDW  R7, [R5 + #0x40]
    LDW  R7, [R6 + #0xE0]      ; 4
    LDW  R7, [R6 + #0xF0]      ; 5
    LDW  R7, [R5 + #0x50]      ; 6
    STW  R7, [R5 + #0x60]      ; 7
    LDW  R7, [R5 + #0x60]      ; 8
    LDW  R7, [R10]             ; 9
    LDW  R7, [R10 + #0x200]    ; 10
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
