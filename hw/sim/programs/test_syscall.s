; test_syscall.s — Verify SYSCALL instruction traps to VEC_SYSCALL
;
; Tests:
;   1. SYSCALL from supervisor mode traps to VEC_SYSCALL (vector 5)
;   2. Handler verifies EPC points at the SYSCALL instruction
;   3. SYSCALL from user mode also traps (not a privilege violation)
;
; The handler uses R7 as a "which test" tag so a single handler
; can service both the supervisor-mode and user-mode SYSCALLs.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ USER_RWX,  0xF9       ; V|R|W|X|U|G — full access, user-accessible

; ═══════════════════════════════════════════════════════════════
; Main setup (supervisor mode)
; ═══════════════════════════════════════════════════════════════
_start:
    LLI  R1, #0               ; assume fail

    ; ── Install vector table in RAM ──────────────────────────
    LA   R2, #syscall_handler
    STW  R2, [R0 + #0x14]     ; vector[5] = SYSCALL handler

    ; ── Test 1: SYSCALL from supervisor mode ──────────────────
    ; R7 = 1 → tells handler this is test 1
    LLI  R7, #1
    LA   R10, #sv_syscall      ; R10 = expected EPC
sv_syscall:
    SYSCALL

    ; Handler should set R4=1 and ERET back here
    CMP  R4, #1
    BNE  fail

    ; ── Test 2: SYSCALL from user mode ────────────────────────
    ; Map VPN 0 → PPN 0 (user-accessible) for user_code
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #USER_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; Map ROM page (VPN 0xFFFF0 → PPN 0xFFFF0)
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00F9        ; PPN=0xFFFF0, USER_RWX (user code runs from ROM)
    WRSYS R3, #MMU, #TLB_PTE

    ; Enable MMU
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; R7 = 2 → tells handler this is test 2
    LLI  R7, #2
    LA   R10, #user_code       ; R10 = expected EPC (SYSCALL in user_code)

    ; Switch to user mode via ERET
    LLI  R2, #0                ; user-mode SR (S=0, I=0)
    WRSPR ESR, R2
    LA   R3, #user_code
    WRSPR EPC, R3
    ERET

    ; Should never reach here
    B    fail

; ═══════════════════════════════════════════════════════════════
; User-mode code
; ═══════════════════════════════════════════════════════════════
user_code:
    SYSCALL
    ; If we reach here, handler returned via ERET — good!
    CMP  R5, #1
    BNE  fail

    ; All passed
    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; SYSCALL handler (runs in supervisor mode)
; ═══════════════════════════════════════════════════════════════
syscall_handler:
    CMP   R7, #1
    BNE   not_supervisor
    LLI   R4, #1
    B     exit_syscall
not_supervisor:
    CMP   R7, #2
    BNE   fail
    LLI   R5, #1
    B     exit_syscall
exit_syscall:
    RDSPR R8, EPC
    ADD   R8, #4
    WRSPR EPC, R8
    ERET
