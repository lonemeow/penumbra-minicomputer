; test_pc_source.s — R15/PC read as a source operand
;
; Reading R15 yields the instruction's own PC (architecture.md): gen1 reads it
; from the regfile, gen2 substitutes the live PC in the ID operand mux. Verify
; both operand positions, self-checking (no absolute-address assumptions):
;   1. operand A / memory base — a PC-relative load of an adjacent literal
;      (the boot ROM's _trap_bus_ignore idiom)
;   2. operand B / ALU source  — two `MOV Rd, PC` one instruction apart; their
;      difference must equal the instruction size
;
; Convention: R1=1 PASS, R1=0 FAIL. On failure the BREAK's PC names the check.

_start:
    LLI  R1, #0                ; assume fail

    ; ── 1: PC-relative load (R15 as the memory base) ──────────
ld_lit:
    LDW  R4, [R15 + #8]        ; R15 = PC of this LDW; +8 lands on 'lit'
    B    past_lit              ; skip the embedded literal word
lit:
    .word 0xCAFEF00D
past_lit:
    LLI  R5, #0xF00D           ; rebuild 0xCAFEF00D for the compare
    LUI  R5, #0xCAFE
    CMP  R4, R5
    BEQ  pcrel_ok
    BREAK
pcrel_ok:

    ; ── 2: MOV Rd, PC reads the current PC (R15 as an ALU source) ──
pc_a:
    MOV  R2, PC                ; R2 = address of pc_a
    MOV  R3, PC                ; R3 = address of this MOV (pc_a + 4)
    SUB  R3, R2                ; R3 = 4 — one instruction apart
    CMP  R3, #4
    BEQ  movpc_ok
    BREAK
movpc_ok:

    LLI  R1, #1                ; PASS
    BREAK
