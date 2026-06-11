; test_divmul_store.s — gen2 hazard probe: a memory op immediately behind
; a dual-write divmul must not drop the divmul's high-half (Rdh) write.
;
; A divmul (MUL/DIV) commits two registers — Rd (low) and Rdh (high) — through
; the single regfile write port over two consecutive WB cycles, holding MEM via
; WB's o_stall across the second (aux) cycle. If a load/store sits immediately
; behind the divmul, it lands fresh in MEM exactly as the divmul reaches WB and
; begins that 2-cycle hold. The MEM stage must defer the memory op's launch
; while WB back-pressures — otherwise the launch-cycle MEM/WB bubble overwrites
; the divmul slot mid-sequence and the Rdh write is lost.
;
; MULU 0x00020003 × 0x00010000 = 0x0000_0002_0003_0000:
;   low  half (R3) = 0x00030000
;   high half (R5) = 0x00000002   <- this is the aux write at risk
; The trailing LDW (no dependency on the divmul outputs, so it issues right
; behind it) is the memory op that probes the hazard. Self-checks into R1
; (1 = PASS, 0 = FAIL); run via tb_penumbra2_prog.

_start:
    LLI  R1, #0                 ; assume FAIL
    LLI  R7, #0x100             ; a valid data address for the trailing load

    ; ── Build the MULU operands ─────────────────────────────────
    LLI  R3, #0x0003
    LUI  R3, #0x0002           ; R3 = 0x00020003 (multiplicand)
    LLI  R4, #0x0000
    LUI  R4, #0x0001           ; R4 = 0x00010000 (multiplier)

    ; ── The critical adjacency: dual-write divmul, then a load ──
    MULU R3, R4, R5            ; R5:R3 = R3 × R4 → R3=0x00030000, R5=0x00000002
    LDW  R6, [R7]             ; memory op immediately behind the divmul

    ; ── Check the high half (Rdh / aux) survived ────────────────
    LLI  R8, #0x0002           ; expected R5 = 0x00000002
    CMP  R5, R8
    BNE  fail

    ; ── Check the low half (Rd) too ─────────────────────────────
    LLI  R9, #0x0000
    LUI  R9, #0x0003           ; expected R3 = 0x00030000
    CMP  R3, R9
    BNE  fail

    LLI  R1, #1               ; both halves committed → PASS
fail:
    BREAK
