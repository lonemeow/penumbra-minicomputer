; mmu_inval_reserved.s -- TLB_INVAL_ALL against main slot 5, under the
; MMU version the ISS presents: reserved in version 1, so the entry
; stays; an invalidation in version 2, so it goes.  The MMU is off.
;
; R2  MMU_CONFIG
; R3  slot 5's V bit after the TLB_INVAL_ALL write

.equ TLB_INVAL_ALL,  8
.equ MMU_CONFIG,     15

_start:
    RDSYS R2, #MMU, #MMU_CONFIG
    LLI  R1, #5
    WRSYS R1, #MMU, #TLB_INDEX
    LI   R1, #0x00010505           ; VPN 0x00105, ASID 5
    WRSYS R1, #MMU, #TLB_VPN
    LI   R1, #0x00105019           ; V|R|W
    WRSYS R1, #MMU, #TLB_PTE
    WRSYS R0, #MMU, #TLB_INVAL_ALL
    RDSYS R3, #MMU, #TLB_PTE
    AND  R3, #1
    BREAK
