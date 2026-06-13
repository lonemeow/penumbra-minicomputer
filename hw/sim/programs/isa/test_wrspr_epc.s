; test_wrspr_epc.s — RDSPR/WRSPR EPC and ESR round-trip
;
; The trap-frame value SPRs EPC and ESR are read and written like registers:
; WRSPR stores the Rd value to the SPR, RDSPR reads it back. These commit at
; WB like any register write (no microarchitectural side effect — unlike
; WRSYS), so this is a plain storage round-trip. Distinct patterns catch
; cross-wiring between EPC and ESR; an interleaved read proves a write to one
; does not disturb the other.
;
; No REQUIRES tag: EPC/ESR access is base ISA — it runs on the ISS and every
; core generation. (SCRn/USP live in test_scratch_sprs / test_usp, which
; carry the wrspr tag for the backends those exercise.)
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0                  ; assume FAIL

    ; ── Build distinct patterns ──────────────────────────────
    LLI   R2, #0xBEEF            ; EPC ← 0xDEAD_BEEF
    LUI   R2, #0xDEAD
    LLI   R4, #0x5678            ; ESR ← 0x1234_5678
    LUI   R4, #0x1234

    ; ── EPC round-trip ───────────────────────────────────────
    WRSPR EPC, R2
    RDSPR R3, EPC
    CMP   R3, R2
    BNE   fail

    ; ── ESR round-trip ───────────────────────────────────────
    WRSPR ESR, R4
    RDSPR R5, ESR
    CMP   R5, R4
    BNE   fail

    ; ── Independence: the ESR write must not disturb EPC ─────
    RDSPR R3, EPC
    CMP   R3, R2
    BNE   fail

    ; ── Overwrite EPC; ESR must be unchanged ─────────────────
    LLI   R6, #0x0000            ; EPC ← 0xCAFE_0000
    LUI   R6, #0xCAFE
    WRSPR EPC, R6
    RDSPR R3, EPC
    CMP   R3, R6
    BNE   fail
    RDSPR R5, ESR               ; ESR still 0x1234_5678
    CMP   R5, R4
    BNE   fail

    LLI  R1, #1                  ; PASS
fail:
    BREAK
