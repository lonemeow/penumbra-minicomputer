; test_forward.s — operand-forwarding result-equivalence (gen2.5) / RAW conformance.
;
; Forwarding changes timing, not results. This program stresses the RAW
; distances the gen2.5 forward network and write-through cover, plus the
; load-use and divmul-result paths, and self-checks the VALUES. It must pass
; identically on the ISS, gen1, gen2 (pure stall), and gen2.5 (forwarding): a
; wrong answer here is a mis-wired forward (wrong source selected / stale value),
; the exact bug signature forwarding can introduce. It does not prove a value
; was forwarded vs. stalled — that is the CPI delta, read from the HW perfctrs.
;
; The USP forward (a WRSPR-USP value reaching a back-to-back RDSPR-USP, physical
; entry 14) is already covered by test_wrspr_usp. SPR-file SPRs (EPC/ESR/SCRn)
; are not forwarded in this cut, so they are deliberately not exercised here.
;
; Result: R1=1 PASS, R1=0 FAIL. Scratch data at 0x100+, clear of the code.

_start:
    LLI  R1, #0                 ; assume FAIL until the final PASS
    LLI  R3, #0x100             ; scratch data base (word-aligned)

    ; ── Back-to-back ALU RAW: EX->EX forward on BOTH operands (d=1) ──
    ; Doubling chain 1->2->4->8->16. Each ADD R4,R4 reads, on op_a and op_b at
    ; once, the value its immediate predecessor produced — so a broken EX->EX
    ; leg (either operand) lands a wrong sum.
    LLI  R4, #1
    ADD  R4, R4                 ; 2
    ADD  R4, R4                 ; 4
    ADD  R4, R4                 ; 8
    ADD  R4, R4                 ; 16
    CMP  R4, #16
    BNE  fail

    ; ── Distance-2 (MEM->EX) ────────────────────────────────────────
    ; Producer, one independent filler, then the consumer: the producer is in
    ; WB when the consumer reaches EX, so the value rides the MEM/WB forward.
    LLI  R5, #50
    MOV  R6, R5                 ; producer: R6 = 50
    LLI  R7, #0                 ; filler (independent)
    ADD  R7, R6                 ; R7 = 0 + 50 = 50  (R6 consumed at distance 2)
    CMP  R7, #50
    BNE  fail

    ; ── Distance-3 (WB->ID write-through) ───────────────────────────
    ; Two fillers between producer and consumer: the producer retires the same
    ; cycle the consumer reads it in ID, so only the regfile write-through hits.
    MOV  R8, R5                 ; producer: R8 = 50
    LLI  R9, #0                 ; filler 1
    LLI  R10, #0                ; filler 2
    ADD  R10, R8               ; R10 = 0 + 50 = 50  (R8 consumed at distance 3)
    CMP  R10, #50
    BNE  fail

    ; ── Load-use: MEM->EX forward of a loaded value (1-cycle interlock) ──
    LLI  R11, #0x42
    STW  R11, [R3]             ; mem[0x100] = 0x42
    LDW  R12, [R3]             ; load it back
    ADD  R12, R11             ; consumer right behind the load: R12 = 0x42 + 0x42
    CMP  R12, #0x84
    BNE  fail

    ; ── Store-data forward: a just-produced value as the stored datum ───
    LLI  R4, #0x55
    ADD  R4, R11              ; producer: R4 = 0x55 + 0x42 = 0x97
    STW  R4, [R3 + #4]        ; store R4 immediately — forwards the store datum
    LDW  R5, [R3 + #4]
    CMP  R5, #0x97
    BNE  fail

    ; ── Divmul result forward: consume the low half right after MUL ─────
    LLI  R6, #6
    LLI  R7, #7
    MUL  R6, R7, R9           ; R6 = 42 (low half), R9 = high half (0, unread)
    MOV  R8, R6              ; forward the divmul low half (d=1, EX/MEM)
    CMP  R8, #42
    BNE  fail

    ; ── All forwarded values were correct ──────────────────────────
    LLI  R1, #1                ; PASS
fail:
    BREAK
