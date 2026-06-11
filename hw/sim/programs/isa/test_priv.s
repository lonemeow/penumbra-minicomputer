; test_priv.s — Privilege violation trap for user-mode system instructions
; REQUIRES: mmu wrspr
;
; Tests:
;   1. Set up identity-mapped TLB (page 0 with U bit for user code)
;   2. Enable MMU, ERET to user mode
;   3. User-mode code attempts DI (privileged) → should trap to VEC_PRIV
;   4. Privilege handler verifies EPC points at the faulting DI,
;      sets R1=1 (pass), then does BREAK
;   5. If DI actually executes (no trap), user code falls through to fail
;
; Key verification: the DI instruction must NOT execute — SR.I must remain
; unchanged. The trap fires at dispatch time, before any micro-ops run.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ USER_RWX,  0xF9       ; V|R|W|X|U|G — full access, user-accessible

; ═══════════════════════════════════════════════════════════════
; Main setup (supervisor mode)
; ═══════════════════════════════════════════════════════════════
_start:
    LLI  R1, #0               ; assume fail

    ; ── Install vector table in RAM ─────────────────────────
    LA   R2, #priv_handler
    STW  R2, [R0 + #16]       ; vector[4] = privilege violation (0x10)

    ; ── Map VPN 0 → PPN 0 (user-accessible) ──────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #USER_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map ROM page (VPN 0xFFFF0 → PPN 0xFFFF0) ───────────
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00F9        ; PPN=0xFFFF0, USER_RWX (user code runs from ROM)
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ────────────────────────────────────────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; ── Remember where user_code's DI is for handler to check ─
    LA   R10, #user_code       ; R10 = expected EPC (DI addr)

    ; ── Switch to user mode via ERET ──────────────────────────
    LLI  R2, #0                ; user-mode SR (S=0, I=0)
    WRSPR ESR, R2
    LA   R3, #user_code
    WRSPR EPC, R3
    ERET

    ; Should never reach here
    B    fail

; ═══════════════════════════════════════════════════════════════
; Privilege violation handler (runs in supervisor mode)
; Verifies EPC matches the faulting DI, then passes.
; ═══════════════════════════════════════════════════════════════
priv_handler:
    RDSPR R11, EPC
    CMP   R11, R10
    BNE   fail
    LLI   R1, #1
    BREAK

; ═══════════════════════════════════════════════════════════════
; User-mode code (runs on identity-mapped page 0 with U bit)
; ═══════════════════════════════════════════════════════════════
user_code:
    ; Attempt DI — this is privileged, should trap to priv_handler
    DI

    ; If we reach here, DI executed without trapping — FAIL
    B    fail

fail:
    BREAK
