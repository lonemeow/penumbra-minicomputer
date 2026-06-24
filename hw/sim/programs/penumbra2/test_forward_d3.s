; test_forward_d3.s — a load result consumed at distance 2 and 3, on BOTH
; operand ports, cache-hot.
; REQUIRES: mmu cache
;
; The pattern that breaks Dhrystone's scanf: a loaded value (often a pointer) is
; consumed a few instructions later — as op_b (an ALU source) or op_a (a base
; address / ALU dst-source). d=2 is served by the MEM->EX forward, d=3 by the
; WB->ID write-through. memcpy only has d=1 deps and the uncached suite never
; forwards, so these legs were unexercised, op_a (the EA path) especially.
; Cacheable code + warm loops make the producer/consumer land at the intended
; distance and forward; the accumulated sums catch a stale value.
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD     ; identity-map the ROM page, cacheable (V|C|R|W|X|G)

_start:
    LLI  R1, #0

    ; ── Map the ROM code page cacheable, enable MMU + I-cache ──
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #ICACHE, #CACHE_CTRL

    LA   R3, konst                  ; &konst (a cacheable ROM word = 5)
    LLI  R6, #1

    ; ── op_b at d=2 (MEM->EX) ──
    LLI  R5, #0
    LLI  R7, #16
loop_b2:
    LDW  R8, [R3]                   ; (P) R8 = 5
    LLI  R9, #0                     ; filler
    ADD  R5, R8                     ; (C) R5 += R8 (R8 is op_b, d=2)
    SUB  R7, #1
    BNE  loop_b2
    CMP  R5, #80                    ; 16 * 5
    BNE  fail

    ; ── op_b at d=3 (WB->ID write-through) ──
    LLI  R5, #0
    LLI  R7, #16
loop_b3:
    LDW  R8, [R3]                   ; (P) R8 = 5
    LLI  R9, #0                     ; filler 1
    LLI  R10, #0                    ; filler 2
    ADD  R5, R8                     ; (C) R5 += R8 (R8 is op_b, d=3)
    SUB  R7, #1
    BNE  loop_b3
    CMP  R5, #80
    BNE  fail

    ; ── op_a at d=2 (the loaded value as an ALU dst-source / EA base) ──
    LLI  R5, #0
    LLI  R7, #16
loop_a2:
    LDW  R8, [R3]                   ; (P) R8 = 5
    LLI  R9, #0                     ; filler
    SUB  R8, R6                     ; (C) R8 = R8 - 1 = 4 (R8 is op_a, d=2)
    ADD  R5, R8
    SUB  R7, #1
    BNE  loop_a2
    CMP  R5, #64                    ; 16 * 4
    BNE  fail

    ; ── op_a at d=3 (WB->ID write-through into the op_a / base path) ──
    LLI  R5, #0
    LLI  R7, #16
loop_a3:
    LDW  R8, [R3]                   ; (P) R8 = 5
    LLI  R9, #0                     ; filler 1
    LLI  R10, #0                    ; filler 2
    SUB  R8, R6                     ; (C) R8 = R8 - 1 = 4 (R8 is op_a, d=3)
    ADD  R5, R8
    SUB  R7, #1
    BNE  loop_a3
    CMP  R5, #64                    ; 16 * 4
    BNE  fail

    LLI  R1, #1                     ; every leg delivered the loaded value → PASS
fail:
    BREAK

konst: .word 5
