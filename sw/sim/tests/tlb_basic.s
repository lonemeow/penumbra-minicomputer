; tlb_basic.s -- main-TLB translations whose shadow outcomes are the same in
; every TLB layout.
;
; Code runs from a pinned slot, which the TLB model never sees; data pages
; are refilled on demand by a miss handler that maps the faulting page to
; itself in main-TLB way 0.  Each page's second load follows its first
; with no other install between them, so no victim choice can matter:
;
;   A  two loads from each of four pages (VPNs 0x101 0x102 0x104 0x108),
;      in supervisor mode: the first misses everywhere, the second hits.
;   B  a load from VPN 0x103; software invalidates its real slot (set 3,
;      way 0, clearing the staged VPN too); the load again.  The shadows
;      drop the entry with the real TLB, so both loads miss.
;   C  in user mode, one load from each of VPNs 0x10A and 0x10B.
;
; Every shadow and the real TLB: 6 supervisor misses and 2 user misses.

.equ ROM_PTE,       0xFFFF00E9     ; V|R|X|U|G, uncached
.equ SUPER_FLAGS,   0x19           ; V|R|W
.equ USER_FLAGS,    0x59           ; V|R|W|U
.equ USER_SR,       0x4000         ; upper half: S=0

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

    LLI  R8, #SUPER_FLAGS
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; A
    LI   R5, #0x101000
    BL   load_twice
    LI   R5, #0x102000
    BL   load_twice
    LI   R5, #0x104000
    BL   load_twice
    LI   R5, #0x108000
    BL   load_twice

    ; B
    LI   R5, #0x103000
    LDW  R7, [R5]
    LLI  R2, #3
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R0, #MMU, #TLB_VPN
    WRSYS R0, #MMU, #TLB_PTE
    LDW  R7, [R5]

    ; C
    LLI  R8, #USER_FLAGS
    LLI  R2, #0
    LUI  R2, #USER_SR
    WRSPR ESR, R2
    LA   R2, #user_entry
    WRSPR EPC, R2
    ERET

user_entry:
    LI   R5, #0x10A000
    LDW  R7, [R5]
    LI   R5, #0x10B000
    LDW  R7, [R5]
    BREAK

load_twice:
    LDW  R7, [R5]
    LDW  R7, [R5]
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
