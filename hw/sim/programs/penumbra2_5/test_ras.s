; test_ras.s — gen2.5 return-address-stack (RAS) integration test.
;
; Exercises the RAS end-to-end through ID (call/return detect, push/pop, return
; redirect) -> spine -> EX (indirect-target verify) -> core redirect — wiring the
; penumbra2_ras unit test cannot reach. The RAS is a performance hint guarded by
; EX, so every check is result-based: a prediction never changes the
; architectural outcome, and a wrong hint must be corrected by EX.
;
; Cases:
;   1. nested calls/returns — main -> f1 -> f2 (leaf), f1 saving its link across
;      the nested call. Each RET (JMP R13) is RAS-predicted; the tree must unwind
;      to the right places.
;   2. clobbered return target — a callee rewrites R13 before RET, so the RAS's
;      predicted target (the pushed return addr) differs from the real target.
;      EX's target check must catch the mismatch and redirect to the actual R13.
;      This is the one case that exercises target_wrong: if it is broken, the RAS
;      hint stands and control lands on the poison at the stale predicted addr.
;
; Self-checks into R1 (1 = PASS, 0 = FAIL); run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                ; assume FAIL until a test proves otherwise
    LLI  R3, #0
    LLI  R4, #0

    ; ── Test 1: nested calls, every return RAS-predicted ──
    BL   f1                    ; f1 calls leaf f2, then sets R4
    CMP  R3, #99               ; the leaf's result must have survived
    BNE  fail
    CMP  R4, #7                ; f1's post-nested-return work must have run
    BNE  fail

    ; ── Test 2: a callee clobbers its own return target ──
    ; This BL pushes the return addr (= chk2_poison) onto the RAS. clobber_fn
    ; rewrites R13 to 'redirected' and RETs, so the RAS predicts chk2_poison
    ; while the real target is 'redirected'. EX's target check must win.
    BL   clobber_fn
chk2_poison:
    LLI  R1, #0                ; poison: EX did not correct the stale RAS target
    BREAK

redirected:
    LLI  R1, #1                ; PASS — target_wrong fired; EX used the real R13
    BREAK

fail:
    BREAK                      ; R1 = 0 → FAIL

; ── Subroutines ──────────────────────────────────────────
f1:
    MOV  R6, R13               ; save link — the nested BL will clobber R13
    BL   f2
    LLI  R4, #7
    MOV  R13, R6               ; restore link
    RET                        ; RAS-predicted return into _start

f2:
    LLI  R3, #99
    RET                        ; leaf return into f1 — RAS-predicted

clobber_fn:
    LA   R13, redirected       ; overwrite the return target
    RET                        ; JMP R13: RAS predicts chk2_poison, EX uses R13
