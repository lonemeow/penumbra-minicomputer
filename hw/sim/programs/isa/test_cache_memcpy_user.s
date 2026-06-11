; test_cache_memcpy_user.s — cached memcpy run in USER mode.
; REQUIRES: mmu cache wrspr
;
; Sets up SRC and DST pages as user-accessible, ERETs into user mode
; pointing at a memcpy loop, then SYSCALLs back to the kernel for
; verification.  Tests whether the residual NetBSD-userspace bug
; needs user mode specifically (SR.S=0, IRQs enabled by default,
; banked USP) and not just supervisor-mode cached LDW/STW.
;
; The user code lives in the same ROM page as the kernel
; preamble — that page is mapped user-accessible (PTE.U=1) so the
; user-mode CPU can fetch from it.  Real userland would have its
; own code page; the bug we're chasing is in the *data* path (LDW
; returning wrong values, STW landing at wrong slots, etc.), so
; the location of the code shouldn't matter.
;
; Result: R1 = 1 PASS, R1 = 0 FAIL.

.equ N_WORDS,    1024
.equ SRC_PA,     0x1000
.equ DST_PA,     0x2000
.equ SRC_VA,     0x1000
.equ DST_VA,     0x2000
.equ KERN_RWX_C, 0xBD               ; V|C|R|W|X|G — kernel
.equ USER_RWX_C, 0x7D               ; V|C|R|W|X|U — user-accessible
.equ USER_ROM,   0xF9               ; V|R|W|X|U|G — for ROM page
.equ USER_SR,    0x40000000         ; S=0, I=1
.equ USER_SP,    0x0FFC             ; top of VPN 0, well-aligned

_start:
    LLI  R1, #0

    ; ── Install SYSCALL handler ─────────────────────────────
    LA   R2, #syscall_handler
    STW  R2, [R0 + #0x14]            ; vector[5] = VEC_SYSCALL

    ; ── Phase 1: plant pattern at SRC_PA (MMU off) ──────────
    LLI  R5, #SRC_PA
    LLI  R6, #0
plant_loop:
    LUI  R7, #0x8000
    OR   R7, R6
    STW  R7, [R5]
    ADD  R5, #4
    ADD  R6, #1
    CMP  R6, #N_WORDS
    BNE  plant_loop

    ; ── Phase 2: set up TLB ─────────────────────────────────
    ; Slot 0: VPN 0 kernel scratch (cacheable RWX, kernel only)
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 1: VPN 1 (SRC) user accessible cacheable RWX
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0100                 ; VPN=1, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x107D                 ; PPN=1, V|C|R|W|X|U
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 2: VPN 2 (DST) user accessible cacheable RWX
    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200                 ; VPN=2, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #0x207D
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 16: ROM page user-accessible (U bit set so user can
    ; fetch from it).  Both kernel and user run from here.
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00F9             ; V|R|W|X|U|G
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU + D-cache ────────────────────────────────
    LLI  R6, #1
    WRSYS R6, #MMU, #MMUCR
    WRSYS R6, #DCACHE, #CACHE_CTRL

    ; ── Set up ERET to user mode ────────────────────────────
    ; ESR controls the SR restored on ERET — set S=0 (user),
    ; I=1 (IRQs on).
    LUI   R2, #0x4000                ; R2 = 0x40000000 = USER_SR
    WRSPR ESR, R2

    ; EPC = user entry point
    LA    R2, #user_entry
    WRSPR EPC, R2

    ; USP = user stack pointer (not actually used by the memcpy
    ; but must be valid in case anything spills).
    LLI   R2, #USER_SP
    WRSPR USP, R2

    ERET                              ; → user_entry in user mode

    ; ERET should not return here.  If we land here, ERET went
    ; somewhere wrong.
    BREAK

; ═══════════════════════════════════════════════════════════════
; User-mode entry — runs in SR.S=0 with IRQs enabled.
;
; Does the memcpy loop, then SYSCALLs back to the kernel with
; R1=success-magic, R8=number-of-words-completed so the kernel
; handler can verify.
; ═══════════════════════════════════════════════════════════════
user_entry:
    LLI  R5, #SRC_VA
    LLI  R6, #DST_VA
    LLI  R8, #0
user_memcpy_loop:
    LDW  R7, [R5]
    STW  R7, [R6]
    ADD  R5, #4
    ADD  R6, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  user_memcpy_loop

    ; Verify dst against expected pattern, also in user mode.
    LLI  R5, #DST_VA
    LLI  R8, #0
user_verify_loop:
    LDW  R7, [R5]
    LUI  R9, #0x8000
    OR   R9, R8
    CMP  R7, R9
    BNE  user_fail
    ADD  R5, #4
    ADD  R8, #1
    CMP  R8, #N_WORDS
    BNE  user_verify_loop

    ; All checks passed in user mode.  Trap into the kernel with
    ; R1 = magic value (1) and let the kernel handler set its own
    ; R1 and BREAK.  Carry the count in R8 too as a sanity tag.
    LLI  R1, #1
    SYSCALL
    ; Not reached.

user_fail:
    LLI  R1, #0
    SYSCALL

; ═══════════════════════════════════════════════════════════════
; SYSCALL handler — capture user's R1 and BREAK with it.
; ═══════════════════════════════════════════════════════════════
syscall_handler:
    ; R1 in user mode is preserved across SYSCALL (no banking),
    ; so we can just BREAK with it.
    BREAK
