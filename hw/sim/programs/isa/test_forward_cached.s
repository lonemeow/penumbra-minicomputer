; test_forward_cached.s — operand forwarding exercised with a HOT front end.
;
; Forwarding only fires when consumer and producer are within a pipeline depth
; of each other, which needs back-to-back fetch — a cache-hot I-stream. With the
; I-cache off every fetch is multi-cycle, so a producer retires before its
; consumer issues and the operand always comes from the regfile: the forward
; muxes are never used. (That is why the uncached conformance suite passed while
; cached real code did not.) So this test maps the ROM page cacheable, enables
; the MMU + I-cache, and runs the patterns in a WARM loop: pass 1 fills the
; cache, the hot passes fetch back-to-back and actually forward.
;
; Patterns the uncached tests miss but cached real code (e.g. Dhrystone) hits,
; all checked by value (a mis-forward corrupts the result or hangs the loop):
;   - a back-to-back ALU RAW chain (EX->EX on both operands);
;   - a call/return where BL's link (R13) feeds the callee's JMP R13 close
;     enough to forward — the function-return shape memcpy never exercises.
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.
; REQUIRES: mmu cache

.equ ROM_PTE_C, 0xFFFF00BD     ; identity-map the ROM page, cacheable (V|C|R|W|X|G)

_start:
    LLI  R1, #0                 ; assume FAIL

    ; ── Map the ROM code page cacheable, enable MMU + I-cache ──
    LLI  R2, #16                ; conventional ROM-page TLB slot
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR          ; enable MMU
    WRSYS R4, #ICACHE, #CACHE_CTRL  ; enable I-cache

    ; ── Warm forwarding loop ──────────────────────────────────────
    LLI  R5, #0                     ; ALU accumulator
    LLI  R6, #1
    LLI  R7, #16                    ; iterations (>1 so the I-cache goes hot)
loop:
    ADD  R5, R6                     ; EX->EX RAW chain: each ADD reads the prior R5
    ADD  R5, R6
    ADD  R5, R6
    ADD  R5, R6
    BL   subr                       ; call: link → R13 (RAS push)
    SUB  R7, #1                     ; (the return lands here)
    BNE  loop

    ; R5 = 4 * 16 = 64 only if every EX->EX forward was correct; the loop only
    ; reaches here at all if all 16 returns came back to this SUB.
    CMP  R5, #64
    BNE  fail
    CMP  R7, R0
    BNE  fail

    LLI  R1, #1                     ; PASS
fail:
    BREAK

; A leaf that returns immediately: its JMP R13 reads the link BL just wrote,
; close behind in the hot stream, so the return target rides the forward path.
subr:
    JMP  R13
