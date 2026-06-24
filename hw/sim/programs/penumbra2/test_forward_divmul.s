; test_forward_divmul.s — divmul result forwarding, cache-hot.
; REQUIRES: mmu cache
;
; A divmul writes two physical entries: Rd (low / quotient) forwards like an ALU
; result once the unit finishes and the slot advances to EX/MEM; Rdh (high /
; remainder) is the scoreboard "aux" writer, which is NOT forwarded — a consumer
; of it must stall until it commits, then read the regfile value. Both are
; checked by value in a warm loop (cache-hot fetch, so the consumer is right
; behind the divmul).
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD      ; ROM code page, cacheable

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
    WRSYS R4, #ICACHE, #CACHE_CTRL

    LLI  R10, #16
loop:
    ; ── Rd (low half) forwarded: consumed right after the divmul ────
    LLI  R4, #6
    LLI  R3, #7
    MUL  R4, R3, R5                 ; R4 = 42 (low), R5 = 0 (high)
    MOV  R8, R4                     ; consume Rd — forward from EX/MEM. R8 = 42
    CMP  R8, #42
    BNE  fail

    ; ── Rdh (high half) consumed: aux writer → must stall, then correct ──
    LLI  R2, #0
    LUI  R2, #1                     ; R2 = 0x00010000
    LLI  R3, #0
    LUI  R3, #1                     ; R3 = 0x00010000
    MULU R2, R3, R6                 ; R2 = lo = 0, R6 = hi = 1
    MOV  R9, R6                     ; consume Rdh — aux, stalls then reads 1
    CMP  R9, #1
    BNE  fail

    SUB  R10, #1
    BNE  loop

    LLI  R1, #1
fail:
    BREAK
