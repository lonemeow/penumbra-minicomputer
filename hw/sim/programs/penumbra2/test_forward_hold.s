; test_forward_hold.s — operand held in EX past its producer's drain.
; REQUIRES: mmu cache
;
; Forwarding registers the operand at ID (possibly stale) and overrides it
; combinationally in EX from the in-flight producer. That assumes the consumer
; reaches EX while the producer is still in MEM/WB. But if the consumer is HELD
; in EX by a *separate* multi-cycle MEM access (a store burst, or a slow load in
; front of it), the older producer keeps advancing and RETIRES from WB while the
; consumer waits — the forward then misses and reverts to the stale ID operand,
; which is latched when the consumer finally advances. memcpy never hits this
; (one memory op per iteration); a function prologue / varargs store burst does,
; which is what corrupts Dhrystone.
;
; Here: P produces R8, S is an (uncached, slow) load that holds the pipe, C
; consumes R8 right behind S. With the I-cache hot the three are back-to-back, so
; C is held in EX across P's retirement. If the forward is lost, C sees a stale
; R8 and the running sum is wrong.
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD      ; ROM page cacheable (I-cache); D-side stays uncached → slow loads

_start:
    LLI  R1, #0

    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #ICACHE, #CACHE_CTRL   ; I-cache only — loads below stay uncached (slow)

    LA   R3, konst                   ; a ROM word to load (uncached → multi-cycle MEM)
    LLI  R6, #1
    LLI  R5, #0                      ; accumulator
    LLI  R7, #16
loop:
    ADD  R8, R6                      ; (P) R8 += 1  — producer of R8
    LDW  R9, [R3]                    ; (S) slow uncached load — holds the pipe in MEM
    ADD  R5, R8                      ; (C) R5 += R8 — consumer of R8, held in EX behind S
    SUB  R7, #1
    BNE  loop

    ; R8 = 1..16, so R5 = 1+2+...+16 = 136 iff C saw the live R8 each pass.
    CMP  R5, #136
    BNE  fail
    LLI  R1, #1
fail:
    BREAK

konst: .word 0
