; sb_window.s -- stores and fills whose store-buffer timing is known.
;
; Run with +cachemodel=4k/2:4k/2:64k/4 and +model_start=0xFFFF0800
; +model_stop=0xFFFF0900.  In user mode, from a cacheable code page,
; `body` runs twice: once before the window, which leaves its code in
; L1-I and every buffer empty, and once inside it on data lines nothing
; has touched.  Each pass:
;
;   four stores filling line A, one to line B, one to line C;
;   a load from a line never touched, which misses L1 and L2;
;   a load from line C, which misses both too, the store having written
;   through without allocating;
;   a store to line D, then a load from an uncached page, which waits for
;   every buffer to empty, and a store there, which finds them empty.
;
; The window's one fetch that misses is window_start's own line, with
; every buffer empty.  run_iss_tests.py derives each buffer's waits.

.equ ROM_PTE,   0xFFFF00ED     ; V|C|R|X|U|G: user code, cacheable
.equ DATA_PTE,  0x000010DD     ; PPN 1, V|C|R|W|U|G
.equ DEV_PTE,   0x000020D9     ; PPN 2, V|R|W|U|G: uncached
.equ USER_SR,   0x4000         ; upper half: S=0

_start:
    ; Pinned slot 0: the ROM page, user-executable and cacheable
    LLI  R2, #0x40
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE
    WRSYS R2, #MMU, #TLB_PTE
    ; Main TLB set 1: the data page at VA 0x1000, cacheable
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0100
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #DATA_PTE
    WRSYS R2, #MMU, #TLB_PTE
    ; Main TLB set 2: the page at VA 0x2000, uncached
    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0200
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #DEV_PTE
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    LLI  R2, #0
    LUI  R2, #USER_SR
    WRSPR ESR, R2
    LA   R2, #user_entry
    WRSPR EPC, R2
    ERET

user_entry:
    LLI  R9, #0x2000
    LLI  R8, #0x1000
    LLI  R5, #0x1800
    BL   body
    LLI  R8, #0x1100
    LLI  R5, #0x1840
    B    window_start

; R8: lines A-D, 16 bytes apart; R5: the untouched line; R9: uncached.
body:
    STW  R7, [R8]
    STW  R7, [R8 + #4]
    STW  R7, [R8 + #8]
    STW  R7, [R8 + #12]
    STW  R7, [R8 + #16]
    STW  R7, [R8 + #32]
    LDW  R6, [R5]
    LDW  R6, [R8 + #32]
    STW  R7, [R8 + #48]
    LDW  R6, [R9]
    STW  R7, [R9]
    JMP  R13

.org 0xFFFF0800
window_start:
    BL   body
    B    window_stop

.org 0xFFFF0900
window_stop:
    BREAK
