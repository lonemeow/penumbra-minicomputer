; test_tlb_user.s — User/kernel access control via TLB U bit
;
; Tests:
;   1. Map page 0 with U (user+kernel), page 1 without U (kernel-only)
;   2. ERET to user mode on page 0 — instruction fetch works (has U)
;   3. User-mode read from page 1 → prot fault (no U for user access)
;   4. Handler verifies fault info (user-mode bit set in FAULT_STATUS),
;      remaps page 1 with U, ERETs to retry
;   5. Retried user-mode read succeeds
;   6. User-mode code does BREAK → trap back to supervisor → testbench stops
;
; This tests the fundamental user/kernel memory isolation mechanism:
; pages without the U bit are invisible to user-mode processes.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ USER_RWX,  0xF9       ; V|R|W|X|U|G — full access, user-accessible
.equ KERN_RWX,  0xB9       ; V|R|W|X|G   — full access, kernel-only (no U)
.equ FAULT_PROT, 2         ; FAULT_STATUS[3:0]
.equ FSTAT_R,    0x100     ; bit [8] = read access
.equ FSTAT_USR,  0x800     ; bit [11] = user-mode access

; ═══════════════════════════════════════════════════════════════
; Main setup (supervisor mode)
; ═══════════════════════════════════════════════════════════════
_start:
    LLI  R1, #0               ; assume fail

    ; ── Install vector table in RAM ───────────────────────────
    LA   R2, #prot_handler
    STW  R2, [R0 + #0x0C]     ; vector[3] = protection fault handler

    ; ── Plant sentinel at physical 0x1000 (MMU off) ───────────
    LLI  R2, #0xFACE
    LLI  R3, #0x1000
    STW  R2, [R3]

    ; ── Map VPN 0 → PPN 0 (user-accessible code page) ────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #USER_RWX         ; V|R|W|X|U|G
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map VPN 1 → PPN 1 (kernel-only, no U) ────────────────
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100           ; VPN=1, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x10B9           ; PTE: PPN=1, V|R|W|X|G (no U!)
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map ROM page (VPN 0xFFFFE → PPN 0xFFFFE) ───────────
    LLI  R2, #30
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFFE00
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFFE0F9        ; PPN=0xFFFFE, USER_RWX (user code runs from ROM)
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ────────────────────────────────────────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR

    ; ── Set up handler expectations ───────────────────────────
    LLI  R10, #0x1000          ; expected FAULT_ADDR

    ; ── Switch to user mode via ERET ──────────────────────────
    ; SR: S=0 (user mode), I=0
    ; PC: user_code label
    LLI  R2, #0                ; user-mode SR
    WRSPR ESR, R2
    LA   R3, #user_code
    WRSPR EPC, R3
    ERET

    ; Should never reach here
    B    fail

; ═══════════════════════════════════════════════════════════════
; Protection fault handler (runs in supervisor mode)
;
; Uses R8, R9, R13 as scratch (safe — user code uses R2-R5)
; R10 = expected FAULT_ADDR (set by supervisor before ERET)
; ═══════════════════════════════════════════════════════════════
prot_handler:
    ; Verify FAULT_ADDR = 0x1000
    RDSYS R8, #MMU, #FAULT_ADDR
    CMP   R8, R10
    BNE   fail

    ; Verify fault type = FAULT_PROT
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F
    MOV   R13, R8
    AND   R13, R9
    CMP  R13, #FAULT_PROT
    BNE   fail

    ; Verify user-mode bit is set (bit 11)
    LLI   R9, #FSTAT_USR
    MOV   R13, R8
    AND   R13, R9
    CMP   R13, R9
    BNE   fail

    ; Verify read access bit (bit 8)
    LLI   R9, #FSTAT_R
    MOV   R13, R8
    AND   R13, R9
    CMP   R13, R9
    BNE   fail

    ; Remap page 1 with U bit so user retry succeeds
    LLI   R8, #1
    WRSYS R8, #MMU, #TLB_INDEX
    LLI   R9, #0x0100           ; VPN=1, ASID=0
    WRSYS R9, #MMU, #TLB_VPN
    LLI   R9, #0x10F9           ; PTE: PPN=1, V|R|W|X|U|G (add U)
    WRSYS R9, #MMU, #TLB_PTE

    ; Return to user mode — retries the faulting LDW
    ERET

; ═══════════════════════════════════════════════════════════════
; User-mode code (executes on page 0 which has U bit)
; Uses R2-R5 only (handler uses R8,R9,R13)
; ═══════════════════════════════════════════════════════════════
user_code:
    ; Read from page 1 (kernel-only) — should fault first time,
    ; succeed after handler remaps with U
    LLI  R3, #0x1000
    LDW  R4, [R3]

    ; If we get here, the retry succeeded — verify value
    LLI  R5, #0xFACE
    CMP  R4, R5
    BNE  fail

    ; PASS — user code successfully accessed remapped page
    LLI  R1, #1
    BREAK

fail:
    BREAK
