; test_add.s — ALU and immediate operations
;
; Tests:
;   1. LLI loads a 16-bit immediate
;   2. ADD computes correct result and doesn't clobber sources
;   3. SUB computes correct result
;   4. CMP sets flags without writing Rd
;   5. NOT inverts bits
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0               ; assume fail

    ; ── Test 1: LLI ─────────────────────────────────────────
    LLI  R2, #5
    CMP R2, #5
    BNE  fail

    ; ── Test 2: ADD ─────────────────────────────────────────
    LLI  R3, #3
    ADD  R2, R3               ; R2 = 5 + 3 = 8
    CMP R2, #8
    BNE  fail
    CMP R3, #3               ; R3 unchanged
    BNE  fail

    ; ── Test 3: SUB ─────────────────────────────────────────
    LLI  R4, #10
    LLI  R5, #4
    SUB  R4, R5               ; R4 = 10 - 4 = 6
    CMP R4, #6
    BNE  fail

    ; ── Test 4: CMP (flags only, no write) ──────────────────
    LLI  R6, #7
    CMP R6, #7               ; sets Z=1
    BNE  fail                 ; should not branch (Z=1 → EQ)
    CMP R6, #7               ; verify R6 still 7 (CMP didn't write)
    BNE  fail

    ; ── Test 5: NOT ─────────────────────────────────────────
    LLI  R7, #0
    NOT  R7, R7               ; R7 = ~0 = 0xFFFFFFFF
    CMP R7, #0               ; NOT of zero is non-zero
    BEQ  fail

    ; ── All checks passed ───────────────────────────────────
    LLI  R1, #1               ; PASS
fail:
    BREAK
