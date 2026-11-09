; mmu.inc -- TLB set-up the isa/ tests share, for any MMU_CONFIG version
; and geometry (doc/system/mmu.md). A test includes it after its own code,
; so _start stays first, and calls tlb_init before it touches the TLB.
;
; A main slot is numbered way * SETS + set in every version; what differs
; is SETS and where TLB_INDEX selects the pinned TLB (bit 6 in version 1,
; bit 31 in version 2), both read from MMU_CONFIG. A read of zero reports
; version 1's 8 pinned slots and 32 sets of 2 ways.
;
; The tests map their low pages and the ROM page (VPN 0xFFFF0) in way 0,
; which keeps them apart only with 32 sets or more: there the ROM page's
; set is 16 or above. tlb_init stops a test on a smaller TLB, R1 = 0.
;
;   BL tlb_init      Initialize the TLB, which is undefined after reset:
;                    invalidate every main slot, write TLB_PTE = 0 to every
;                    pinned slot. Leaves SETS - 1 in SCR2 and TLB_INDEX's
;                    pinned select in SCR3, so with no call, which a test's
;                    exception handler can use, way 0's slot for VPN v is
;                        RDSPR Rx, SCR2
;                        AND   Rx, #v          (AND Rx, Ry: the VPN in Ry)
;                    and pinned slot n's index is
;                        RDSPR Rx, SCR3
;                        ADD   Rx, #n
;                    Clobbers R2-R4 and the flags.
;
;   BL tlb_slot      R2 = the main slot that TLB_VPN word R2 (VPN << 8 | ASID)
;                    takes in way R2[31:28]. Keeps every other register but
;                    R13 (and the flags); uses SCR0 and SCR1.
;
; SCR0-SCR3 are the helpers' alone in a test that includes this file.

tlb_init:
    RDSYS R2, #MMU, #MMU_CONFIG
    CMP   R2, R0
    BNE   tlb_init_cfg
    LI    R2, #0x10C11481           ; zero reports version 1's baseline
tlb_init_cfg:
    MOV   R3, R2
    SHR   R3, #10
    AND   R3, #0x1F                 ; SETS_L2
    CMP   R3, #5
    BHS   tlb_init_sets
    LLI   R1, #0                    ; fewer than 32 sets
    BREAK
tlb_init_sets:
    LLI   R4, #1
    SHL   R4, R3
    SUB   R4, #1
    WRSPR SCR2, R4                  ; SETS - 1
    MOV   R3, R2
    AND   R3, #0x0F                 ; VER
    CMP   R3, #2
    BEQ   tlb_init_v2

    ; Version 1: no invalidation register; write TLB_PTE = 0 slot by slot,
    ; SETS * WAYS of them.
    LLI   R3, #0x40
    WRSPR SCR3, R3
    MOV   R4, R2
    SHR   R4, #10
    AND   R4, #0x1F                 ; SETS_L2
    MOV   R3, R2
    SHR   R3, #15
    AND   R3, #0x1F                 ; WAYS
    SHL   R3, R4                    ; R3 = SETS * WAYS
    LLI   R4, #0
tlb_init_v1_slot:
    WRSYS R4, #MMU, #TLB_INDEX
    WRSYS R0, #MMU, #TLB_PTE
    ADD   R4, #1
    CMP   R4, R3
    BLO   tlb_init_v1_slot
    B     tlb_init_pinned

tlb_init_v2:
    LI    R3, #0x80000000
    WRSPR SCR3, R3
    WRSYS R0, #MMU, #TLB_INVAL_ALL

    ; Every pinned slot: a pinned hit wins over everything.
tlb_init_pinned:
    MOV   R3, R2
    SHR   R3, #4
    AND   R3, #0x3F                 ; PINNED
    LLI   R4, #0
tlb_init_pinned_slot:
    RDSPR R2, SCR3
    ADD   R2, R4
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R0, #MMU, #TLB_PTE
    ADD   R4, #1
    CMP   R4, R3
    BLO   tlb_init_pinned_slot
    RET

tlb_slot:
    WRSPR SCR0, R3
    WRSPR SCR1, R4
    RDSYS R3, #MMU, #MMU_CONFIG
    CMP   R3, R0
    BNE   tlb_slot_cfg
    LI    R3, #0x10C11481           ; zero reports version 1's baseline
tlb_slot_cfg:
    SHR   R3, #10
    AND   R3, #0x1F                 ; R3 = SETS_L2
    MOV   R4, R2
    SHR   R4, #28
    SHL   R4, R3                    ; R4 = way * SETS
    RDSPR R3, SCR2                  ; R3 = SETS - 1
    SHR   R2, #8                    ; the VPN, the way above it
    AND   R2, R3                    ; R2 = set
    ADD   R2, R4
    RDSPR R3, SCR0
    RDSPR R4, SCR1
    RET
