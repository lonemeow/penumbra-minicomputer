; test_wrspr_sr_illegal.s — WRSPR SR is a reserved encoding: it must trap.
;
; SR.S/SR.I change via exception entry / ERET / EI / DI and NZCV via
; flag-writing ALU ops, so software never needs a direct SR write. WRSPR SR
; (SPR number 3) is reserved and traps to VEC_ILLEGAL on every generation.
;
; The handler sets the PASS flag and BREAKs — it does not skip-and-resume, so
; the test needs no WRSPR EPC (no value-SPR write). That keeps it free of the
; `wrspr` capability and runnable on the ISS and every core generation.
; (test_rdspr_sr covers the still-valid RDSPR SR + EI/DI.)
;
; Result: R1=1 PASS (trap fired), R1=0 FAIL (no trap, or wrong vector).

_start:
    LLI   R1, #0                 ; assume FAIL
    RDSPR R2, SR                 ; a benign (supervisor) value to feed WRSPR

    ; install the VEC_ILLEGAL (7) handler in the RAM vector table
    LA    R3, illegal_handler
    STW   R3, [R0 + #0x1C]       ; vector[7] = VEC_ILLEGAL << 2

    ; WRSPR SR must trap before any SR write. If it (wrongly) executed, R2
    ; just rewrites the current SR (S preserved) and we fall through to the
    ; FAIL BREAK still in supervisor mode.
    WRSPR SR, R2

    ; reached only if the trap did not fire
    BREAK                        ; R1 still 0 -> FAIL

illegal_handler:
    LLI   R1, #1                 ; PASS: WRSPR SR trapped to VEC_ILLEGAL
    BREAK
