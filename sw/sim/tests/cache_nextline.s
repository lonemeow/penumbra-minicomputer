; cache_nextline.s -- L1-I misses whose next-line prediction outcomes are
; known.
;
; Everything runs from the uncached ROM page except the code on one
; cacheable page at 0xFFFF1000, entered with BL and left with JMP R13:
; `run`, eight lines fetched straight through (seven of NOPs, then the
; return); `r0` and `r1`, adjacent lines at 0xFFFF1200, each only a
; return; and `last`, the page's last line.  In order:
;
;   1  run, cold: 8 misses to memory, each looking up the next line and
;      finding it missing from L2.
;   2  r1, cold: a miss to memory; the line after it is missing from L2.
;   3  L1-I INVAL_ALL.
;   4  run, warm: 8 misses L2 serves.  "Once" would serve lines 1, 3, 5
;      and 7 from the buffer, "continue" lines 1 to 7; line 7's look-up
;      of the line after the run misses L2.
;   5  r0, cold: a miss to memory whose look-up finds r1 in L2.
;   6  L1-I INVAL_ALL, discarding the r1 prediction unused.
;   7  r1: a miss L2 serves, the buffer cleared; the line after it is
;      missing from L2.
;   8  last, cold: a miss to memory whose next line is in the next page,
;      so there is no look-up.
;
; run_iss_tests.py derives the counts.

.equ ROM_PTE,   0xFFFF00A9     ; V|R|X|G, uncached
.equ CODE_PTE,  0xFFFF10AD     ; V|C|R|X|G

_start:
    ; Pinned slot 0: the ROM page, uncached
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE
    ; Pinned slot 1: the code page, cacheable
    LLI  R2, #0x41
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF100
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #CODE_PTE
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    BL   run                            ; 1
    BL   r1                             ; 2
    WRSYS R4, #ICACHE, #CACHE_INVAL_ALL ; 3
    BL   run                            ; 4
    BL   r0                             ; 5
    WRSYS R4, #ICACHE, #CACHE_INVAL_ALL ; 6
    BL   r1                             ; 7
    BL   last                           ; 8
    BREAK

.org 0xFFFF1000
run:
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    NOP
    JMP  R13

.org 0xFFFF1200
r0:
    JMP  R13
.org 0xFFFF1210
r1:
    JMP  R13

.org 0xFFFF1FF0
last:
    JMP  R13
