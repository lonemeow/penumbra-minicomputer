; mmu_v2.s -- MMU interface version 2, as +mmu=2:256x2 presents it.
;
; The MMU is off and the program supervisor, so WRSYS and RDSYS reach the
; TLB arrays without translating anything.  Checks MMU_CONFIG, the
; version-2 TLB_INDEX (a main slot numbered way * 256 + set, the pinned
; select in bit 31, a slot past the end selecting nothing) and the three
; invalidation registers (doc/system/mmu.md).  The expected values live
; in run_iss_tests.py.
;
; R2  MMU_CONFIG
; R3  TLB_VPN and R4 TLB_PTE of main slot 0x123 (set 0x23, way 1)
; R5  TLB_PTE of pinned slot 2
; R6  TLB_PTE of slot 512, written just before, OR slot 0's
; R7  after TLB_INVAL_VA, a bit for each of e0..e4 still valid
; R8  after TLB_INVAL_ASID, a bit for each of f0..f2 still valid
; R9  after TLB_INVAL_ALL: e1, f1, f2, slot 0x123, then pinned slot 2
; R11 TLB_INDEX, and R12 the TLB_VPN word it then commits, across the
;     invalidations

.equ TLB_INVAL_VA,   6
.equ TLB_INVAL_ASID, 7
.equ TLB_INVAL_ALL,  8
.equ MMU_CONFIG,     15
.equ FLAGS,          0x19          ; V|R|W
.equ GFLAGS,         0x99          ; V|R|W|G

_start:
    RDSYS R2, #MMU, #MMU_CONFIG

    ; A main slot and a pinned one, read back
    LLI  R10, #0x123
    LI   R11, #0x01012305          ; VPN 0x10123, ASID 5
    LI   R12, #0x10123019
    BL   install
    RDSYS R3, #MMU, #TLB_VPN
    RDSYS R4, #MMU, #TLB_PTE

    LI   R10, #0x80000002
    LI   R11, #0x0FFFD000
    LI   R12, #0x00042099
    BL   install
    RDSYS R5, #MMU, #TLB_PTE

    ; Slot 512 is past the end: the write lands nowhere
    LLI  R10, #0x200
    LI   R11, #0x00020005
    LI   R12, #0x00200019
    BL   install
    RDSYS R6, #MMU, #TLB_PTE
    LLI  R10, #0
    WRSYS R10, #MMU, #TLB_INDEX
    RDSYS R1, #MMU, #TLB_PTE
    OR   R6, R1

    ; TLB_INVAL_VA
    ;   e0  set 0x40 way 0, VPN 0x00140 ASID 3   -- named: goes
    ;   e1  set 0x40 way 1, VPN 0x00240 ASID 3   -- same set: stays
    ;   e2  set 0x41 way 0, VPN 0x00141 ASID 4   -- named under ASID 3: stays
    ;   e3  set 0x42 way 0, VPN 0x00142 global   -- named under ASID 7: goes
    ;   e4  set 0x44 way 0, VPN 0x00143 ASID 3   -- outside its set: stays
    LLI  R10, #0x040
    LI   R11, #0x00014003
    LI   R12, #0x00140019
    BL   install
    LLI  R10, #0x140
    LI   R11, #0x00024003
    LI   R12, #0x00240019
    BL   install
    LLI  R10, #0x041
    LI   R11, #0x00014104
    LI   R12, #0x00141019
    BL   install
    LLI  R10, #0x042
    LI   R11, #0x00014200
    LI   R12, #0x00142099
    BL   install
    LLI  R10, #0x044
    LI   R11, #0x00014303
    LI   R12, #0x00143019
    BL   install
    LI   R1, #0x00014003
    WRSYS R1, #MMU, #TLB_INVAL_VA
    LI   R1, #0x00014103
    WRSYS R1, #MMU, #TLB_INVAL_VA
    LI   R1, #0x00014207
    WRSYS R1, #MMU, #TLB_INVAL_VA
    LI   R1, #0x00014303
    WRSYS R1, #MMU, #TLB_INVAL_VA
    LLI  R7, #0
    LLI  R10, #0x040
    BL   valid
    OR   R7, R1
    LLI  R10, #0x140
    BL   valid
    SHL  R1, #1
    OR   R7, R1
    LLI  R10, #0x041
    BL   valid
    SHL  R1, #2
    OR   R7, R1
    LLI  R10, #0x042
    BL   valid
    SHL  R1, #3
    OR   R7, R1
    LLI  R10, #0x044
    BL   valid
    SHL  R1, #4
    OR   R7, R1

    ; TLB_INVAL_ASID 3
    ;   f0  ASID 3          -- goes
    ;   f1  ASID 4          -- stays
    ;   f2  ASID 3, global  -- stays
    LLI  R10, #0x050
    LI   R11, #0x00015003
    LI   R12, #0x00150019
    BL   install
    LLI  R10, #0x150
    LI   R11, #0x00025004
    LI   R12, #0x00250019
    BL   install
    LLI  R10, #0x051
    LI   R11, #0x00015103
    LI   R12, #0x00151099
    BL   install
    LLI  R1, #3
    WRSYS R1, #MMU, #TLB_INVAL_ASID
    LLI  R8, #0
    LLI  R10, #0x050
    BL   valid
    OR   R8, R1
    LLI  R10, #0x150
    BL   valid
    SHL  R1, #1
    OR   R8, R1
    LLI  R10, #0x051
    BL   valid
    SHL  R1, #2
    OR   R8, R1

    ; TLB_INVAL_ALL, with a slot selected and a word staged around it
    LLI  R1, #0x77
    WRSYS R1, #MMU, #TLB_INDEX
    LI   R1, #0x0ABCDE05
    WRSYS R1, #MMU, #TLB_VPN
    WRSYS R0, #MMU, #TLB_INVAL_ALL
    RDSYS R11, #MMU, #TLB_INDEX
    LI   R1, #0x0ABCD019
    WRSYS R1, #MMU, #TLB_PTE
    RDSYS R12, #MMU, #TLB_VPN
    LLI  R9, #0
    LLI  R10, #0x140
    BL   valid
    OR   R9, R1
    LLI  R10, #0x150
    BL   valid
    SHL  R1, #1
    OR   R9, R1
    LLI  R10, #0x051
    BL   valid
    SHL  R1, #2
    OR   R9, R1
    LLI  R10, #0x123
    BL   valid
    SHL  R1, #3
    OR   R9, R1
    LI   R10, #0x80000002
    BL   valid
    SHL  R1, #4
    OR   R9, R1
    BREAK

; Commit TLB_VPN word R11 and TLB_PTE R12 to slot R10.
install:
    WRSYS R10, #MMU, #TLB_INDEX
    WRSYS R11, #MMU, #TLB_VPN
    WRSYS R12, #MMU, #TLB_PTE
    RET

; R1 = slot R10's V bit.
valid:
    WRSYS R10, #MMU, #TLB_INDEX
    RDSYS R1, #MMU, #TLB_PTE
    AND  R1, #1
    RET
