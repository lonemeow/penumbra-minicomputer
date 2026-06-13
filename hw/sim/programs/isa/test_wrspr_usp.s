; test_wrspr_usp.s — RDSPR/WRSPR USP round-trip (supervisor)
;
; USP is the user stack pointer — the regfile R14 *user* bank (physical entry
; 14), distinct from the supervisor R14/SSP (entry 15). RDSPR/WRSPR USP reach
; entry 14 in any mode (cross_bank), so from supervisor they read and write
; the user SP without disturbing the live supervisor R14. Checks the
; round-trip and that the supervisor R14 is untouched.
;
; Base ISA, no MMU/user mode (test_usp covers user mode seeing USP as its R14,
; REQUIRES mmu wrspr). Runs on the ISS and every core generation.
;
; Result: R1=1 PASS, R1=0 FAIL

_start:
    LLI  R1, #0                  ; assume FAIL

    ; Snapshot supervisor R14 (SSP) to prove the USP writes leave it alone.
    MOV  R10, R14

    ; ── USP round-trip ──────────────────────────────────────
    LLI   R2, #0xF00D            ; USP ← 0xCAFE_F00D
    LUI   R2, #0xCAFE
    WRSPR USP, R2
    RDSPR R3, USP
    CMP   R3, R2
    BNE   fail

    ; ── Overwrite USP; read back the new value ──────────────
    LLI   R4, #0x5678            ; USP ← 0x1234_5678
    LUI   R4, #0x1234
    WRSPR USP, R4
    RDSPR R5, USP
    CMP   R5, R4
    BNE   fail

    ; ── Supervisor R14 (SSP) untouched by the USP writes ────
    CMP   R14, R10
    BNE   fail

    LLI  R1, #1                  ; PASS
fail:
    BREAK
