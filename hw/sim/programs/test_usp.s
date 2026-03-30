; test_usp.s — Verify RDSPR/WRSPR USP (GETUSP/SETUSP)
;
; Tests:
;   1. WRSPR sets the user stack pointer from supervisor mode
;   2. RDSPR reads back the same value
;   3. User-mode code sees the USP as its R14
;   4. SETUSP from supervisor changes what user mode sees
;
; Result: R1=1 PASS, R1=0 FAIL

.equ USER_RWX,  0xF9       ; V|R|W|X|U|G — full access, user-accessible

; ═══════════════════════════════════════════════════════════════
; Main setup (supervisor mode)
; ═══════════════════════════════════════════════════════════════
_start:
    LLI  R1, #0

    ; ── Install vector table in RAM ───────────────────────────
    LA   R2, #syscall_handler
    STW  R2, [R0 + #0x14]       ; vector[5] = SYSCALL handler

    ; ── Test 1: WRSPR + RDSPR round-trip ──────────────────────
    LLI  R2, #0xBEEF
    WRSPR USP, R2
    RDSPR R3, USP
    CMP   R3, R2
    BNE   fail

    ; ── Test 2: Verify USP != SSP ─────────────────────────────
    ; Current R14 (SSP) should not be 0xBEEF
    LLI  R14, #0x1000        ; set SSP to something different
    RDSPR R3, USP
    CMP   R3, #0xBEEF        ; USP should still be 0xBEEF
    BNE   fail

    ; ── Test 3: User mode sees USP as R14 ─────────────────────
    ; Set USP to a known value, switch to user mode, verify
    LLI  R2, #0x2000
    WRSPR USP, R2

    ; Map VPN 0 for user code
    LLI  R4, #0
    WRSYS R4, #MMU, #TLB_INDEX
    WRSYS R4, #MMU, #TLB_VPN
    LLI  R5, #USER_RWX
    WRSYS R5, #MMU, #TLB_PTE

    ; Map ROM page (VPN 0xFFFFE → PPN 0xFFFFE)
    LLI  R4, #30
    WRSYS R4, #MMU, #TLB_INDEX
    LI   R5, #0x0FFFFE00
    WRSYS R5, #MMU, #TLB_VPN
    LI   R5, #0xFFFFE0F9        ; PPN=0xFFFFE, USER_RWX (user code runs from ROM)
    WRSYS R5, #MMU, #TLB_PTE

    ; Enable MMU
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; Switch to user mode — user_code will copy R14 to R3 and SYSCALL back
    LLI  R2, #0              ; user-mode SR (S=0, I=0)
    WRSPR ESR, R2
    LA   R4, #user_code
    WRSPR EPC, R4
    ERET

    B    fail

fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; User-mode code
; ═══════════════════════════════════════════════════════════════
user_code:
    ; R14 here should be USP (set by supervisor before ERET)
    MOV  R3, R14
    SYSCALL

; ═══════════════════════════════════════════════════════════════
; SYSCALL handler — verifies user R14 via register and RDSPR
; ═══════════════════════════════════════════════════════════════
syscall_handler:
    CMP   R3,  #0x2000
    BNE   fail
    RDSPR R11, USP
    CMP   R11, #0x2000
    BNE   fail
    LLI   R1, #1
    BREAK
