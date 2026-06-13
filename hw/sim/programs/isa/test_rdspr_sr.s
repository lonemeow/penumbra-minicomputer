; test_rdspr_sr.s — RDSPR SR reads the live status word
;
; RDSPR SR returns {S, I, reserved, NZCV}: S/I from the committed status,
; NZCV bypassed from the youngest in-flight flag writer (so it reads like any
; other flag reader). WRSPR SR is reserved — the kernel changes S/I via
; exception entry / ERET / EI / DI, never a direct SR write — so this test
; manipulates I with EI/DI and NZCV with CMP, never WRSPR SR. That keeps it
; base ISA: it runs on the ISS and every core generation.
;
; SR layout (penumbra_pkg.sv): bit 31 = S, bit 30 = I, bits [3:0] = N,Z,C,V
; (so Z is bit 1).
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0                  ; assume FAIL

    ; ── Boot status: S=1 (supervisor), I=0 ──────────────────
    RDSPR R2, SR
    MOV   R3, R2
    SHR   R3, #31                ; isolate S
    CMP   R3, #1
    BNE   fail
    MOV   R3, R2
    SHR   R3, #30
    AND   R3, #1                 ; isolate I
    CMP   R3, #0
    BNE   fail

    ; ── EI sets I; RDSPR SR observes it ─────────────────────
    EI
    RDSPR R2, SR
    MOV   R3, R2
    SHR   R3, #30
    AND   R3, #1
    CMP   R3, #1
    BNE   fail

    ; ── DI clears I ─────────────────────────────────────────
    DI
    RDSPR R2, SR
    MOV   R3, R2
    SHR   R3, #30
    AND   R3, #1
    CMP   R3, #0
    BNE   fail

    ; ── NZCV bypass: a compare's Z reaches RDSPR SR ─────────
    CMP   R5, R5                 ; equal → Z=1 (R5 is whatever; equal to itself)
    RDSPR R2, SR
    MOV   R3, R2
    SHR   R3, #1                 ; isolate Z (SR bit 1)
    AND   R3, #1
    CMP   R3, #1
    BNE   fail

    LLI   R6, #1
    CMP   R6, R0                 ; 1 vs 0 → not equal → Z=0
    RDSPR R2, SR
    MOV   R3, R2
    SHR   R3, #1
    AND   R3, #1
    CMP   R3, #0
    BNE   fail

    ; ── S survives all of the above ─────────────────────────
    RDSPR R2, SR
    SHR   R2, #31
    CMP   R2, #1
    BNE   fail

    LLI  R1, #1                  ; PASS
fail:
    BREAK
