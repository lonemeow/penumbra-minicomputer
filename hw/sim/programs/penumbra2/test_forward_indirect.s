; test_forward_indirect.s — indirect-jump target forwarded from a load, cache-hot.
; REQUIRES: mmu cache
;
; The function-return shapes a real compiler emits, where the jump *target* comes
; from a load and is consumed close behind it:
;   - a NON-RAS indirect jump (computed/loaded target, e.g. a switch/funcptr):
;     EX redirects purely to the forwarded op_a;
;   - a RAS return whose link was restored from the stack (`ldw lr; jmp lr`):
;     the RAS predicts at fetch, and EX verifies against the forwarded op_a — so
;     a wrong forward there breaks a return the RAS had right.
; Both run in a warm loop; a broken target forward sends control to the poison.
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD      ; ROM code page, cacheable

_start:
    LLI  R1, #0

    ; ── Map data page 1 (cacheable) + ROM code page ──
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0100
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #0x10BD
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #DCACHE, #CACHE_CTRL

    ; Seed a code pointer in memory: mem[0x1004] = &cont1.
    LA   R2, cont1
    LLI  R3, #0x1004
    STW  R2, [R3]

    LLI  R10, #16
loop:
    ; ── non-RAS indirect jump, target loaded then used (load-use into op_a) ──
    LLI  R3, #0x1004
    LDW  R8, [R3]                   ; R8 = &cont1 (loaded)
    JMP  R8                         ; non-RAS jump to the forwarded target
    LLI  R1, #0                     ; poison — only reached if the jump target was wrong
    B    done
cont1:
    ; ── RAS return whose link was restored from the stack ──
    BL   subr                       ; call; subr reloads lr and returns here
    SUB  R10, #1
    BNE  loop

    LLI  R1, #1                     ; PASS
done:
    BREAK

; subr saves and reloads its return address, then returns through the reloaded
; lr — a load-use into a RAS-predicted return.
subr:
    LLI  R4, #0x1008
    STW  R13, [R4]                  ; save lr
    LDW  R13, [R4]                  ; restore lr (load-use into the return)
    JMP  R13                        ; RAS return, target forwarded from the load
