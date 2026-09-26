; cache_window.s -- a measurement window opened and closed by user-mode PCs.
;
; Run with +model_start=0xFFFF0800 +model_stop=0xFFFF0900.  In user mode it
; loads 16 lines before the window, the same 16 lines and 16 new ones
; inside it, and 16 more after it.  Only the window's 32 loads may count:
; 16 hits, because the warm-up before the window still filled the cache,
; and 16 misses.  The code page is uncached, so fetches never count.

.equ ROM_PTE,   0xFFFF00E9     ; V|R|X|U|G, uncached
.equ DATA_PTE,  0x000010DD     ; PPN 1, V|C|R|W|U|G
.equ USER_SR,   0x4000         ; upper half: S=0

_start:
    ; Pinned slot 0: the ROM page, user-executable and uncached
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

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #L2, #CACHE_CTRL

    LLI  R2, #0
    LUI  R2, #USER_SR
    WRSPR ESR, R2
    LA   R2, #user_entry
    WRSPR EPC, R2
    ERET

user_entry:
    LLI  R5, #0x1000
    BL   load16
    B    window_start

; Load one word from each of 16 lines from R5 on, advancing R5 past them.
load16:
    LLI  R6, #16
load16_loop:
    LDW  R7, [R5]
    ADD  R5, #16
    SUB  R6, #1
    BNE  load16_loop
    JMP  R13

.org 0xFFFF0800
window_start:
    LLI  R5, #0x1000
    BL   load16
    BL   load16
    B    window_stop

.org 0xFFFF0900
window_stop:
    BL   load16
    BREAK
