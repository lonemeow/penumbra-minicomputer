; pair_ext.s -- what the extension fusion takes: SHL Rd, #n and the
; SHR or SAR Rd, #n behind it.
;
; Run with +pairmodel; the window runs from reset to the BREAK.  A DI,
; which issues alone, follows every probe.  In order:
;
;   A  SHL R5, #24; SAR R5, #24    a byte's sign extension: ext
;   B  SHL R5, #16; SHR R5, #16    a halfword's zero extension: ext
;   C  SHL R5, #31; SAR R5, #31    bit 0 as a mask: ext
;   D  SHL R5, #24; SAR R5, #27    a signed field: unequal distances
;   E  SHL R5, #0; SAR R5, #0      the flags of R5, unchanged: ext
;   F  SHL R5, #24; SAR R6, #24    another register: independent shifts
;   G  SHL R5, #2; ADD R5, R6      shadd's pair, which ext leaves
;   H  MOV R5, R6; SHL; SAR        movop takes the MOV and the SHL when it
;                                  is on; without it the shifts fuse
;   I  SHL R5, R7; SAR R5, #24     a distance in a register: no ext
;
; run_iss_tests.py derives each stage's fusions and pairs.

_start:
    LLI  R6, #0x1234
    DI
    LLI  R7, #3
    DI

    SHL  R5, #24               ; A
    SAR  R5, #24
    DI

    SHL  R5, #16               ; B
    SHR  R5, #16
    DI

    SHL  R5, #31               ; C
    SAR  R5, #31
    DI

    SHL  R5, #24               ; D
    SAR  R5, #27
    DI

    SHL  R5, #0                ; E
    SAR  R5, #0
    DI

    SHL  R5, #24               ; F
    SAR  R6, #24
    DI

    SHL  R5, #2                ; G
    ADD  R5, R6
    DI

    MOV  R5, R6                ; H
    SHL  R5, #24
    SAR  R5, #24
    DI

    SHL  R5, R7                ; I
    SAR  R5, #24
    DI
    BREAK
