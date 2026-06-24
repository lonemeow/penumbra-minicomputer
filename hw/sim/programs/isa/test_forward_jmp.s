; test_forward_jmp.s — forwarding into a NON-RAS indirect jump target (hot front end).
;
; A JMP through R13 is a return: the RAS predicts its target at fetch, so a wrong
; forwarded operand can be masked by a correct prediction. A JMP through any other
; register (a computed target — jump table / switch / function pointer, which
; Dhrystone's enum switch emits) is NOT RAS-predicted: EX redirects purely to the
; forwarded operand. This isolates the JMP-target forward.
;
; Warm loop (cacheable code, I-cache on, so the producer/JMP pair is back-to-back
; and actually forwards): each pass computes the continuation address into R8 and
; jumps to it indirectly. If the d=1 forward of R8 into the jump target is wrong,
; control leaves the loop (poison / garbage) instead of reaching `cont`.
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.
; REQUIRES: mmu cache

.equ ROM_PTE_C, 0xFFFF00BD     ; identity-map the ROM page, cacheable (V|C|R|W|X|G)

_start:
    LLI  R1, #0                 ; assume FAIL

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

    LLI  R7, #16                ; iterations
loop:
    LA   R8, cont               ; (P) compute the indirect target — producer of R8
    JMP  R8                     ; (C) non-RAS indirect jump; target = R8 forwarded d=1
    LLI  R1, #0                 ; poison: only reached if the JMP fell through
    B    fail
cont:
    SUB  R7, #1
    BNE  loop

    CMP  R7, R0                 ; every indirect jump must have reached cont
    BNE  fail
    LLI  R1, #1                 ; PASS
fail:
    BREAK
