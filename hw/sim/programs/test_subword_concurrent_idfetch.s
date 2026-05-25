; test_subword_concurrent_idfetch.s — I+D bus arbiter under sub-word load
;
; Userland code interleaves D-loads (data) and I-fetches (instructions
; from libraries) constantly.  The bus arbiter serializes split-I/D
; traffic onto the single external bus, and `[Sync bus contract]`-era
; back-to-back BUSY→BUSY paths only get exercised under specific
; concurrent-pressure patterns that the existing tests don't reach.
;
; This test forces high-rate alternation: every iteration of the
; halfword loop body does a sub-word D-load AND calls a subroutine in
; a separate cacheable RAM region (cold-fillable on demand via
; invalidate).  The compute subroutine doesn't do much itself — its
; purpose is to force an I-fetch from a different cache line than
; the loop body, exercising arbiter alternation while sub-word D
; traffic is in flight.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C,  0xBD
.equ DATA_BASE,   0x0100        ; data region
.equ DATA_END,    0x0500        ; 1 KiB, 512 halfwords
.equ FUNC_DEST,   0x0800        ; RAM copy of compute_fn (~2 KB away)

_start:
    LLI  R1, #0

    ; ── MMU setup ────────────────────────────────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE

    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #0xFFFF00B9
    WRSYS R2, #MMU, #TLB_PTE

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Initialize data: A[i] = i + 1 ────────────────────────────
    ; Done with caches still disabled so SDRAM holds the values.
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #1
init_data:
    STH  R7, [R5]
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  init_data

    ; ── Copy compute_fn from ROM into cacheable RAM ──────────────
    LA   R2, #compute_fn
    LLI  R3, #FUNC_DEST
    LA   R4, #compute_fn_end
copy_func:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy_func

    ; ── Enable all caches ────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #L2,     #CACHE_CTRL

    ; ── Invalidate caches so first iter cold-fills both paths ────
    WRSYS R0, #DCACHE, #CACHE_INVAL_ALL
    WRSYS R0, #ICACHE, #CACHE_INVAL_ALL
    WRSYS R0, #L2,     #CACHE_INVAL_ALL

    ; ── Loop: LDH, call compute_fn, advance pointer ──────────────
    ; Each iteration:
    ;   - D-load (sub-word) of A[i] at DATA region
    ;   - I-fetch of compute_fn at FUNC_DEST (different cache lines)
    ;   - return via JMP r13
    LLI  R5, #DATA_BASE
    LLI  R6, #DATA_END
    LLI  R7, #1
    LLI  R8, #FUNC_DEST
walk:
    LDH  R2, [R5]
    CMP  R2, R7
    BNE  fail
    LA   R13, #after_call
    JMP  R8                     ; call compute_fn @ FUNC_DEST
after_call:
    CMP  R9, R7                 ; compute_fn returns R9 = arg (echo)
    BNE  fail
    ADD  R5, #2
    ADD  R7, #1
    CMP  R5, R6
    BNE  walk

    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; compute_fn — small subroutine, copied into cacheable RAM at
; FUNC_DEST.  Takes R7 in, returns R9 = R7.  Does an extra LDH so
; the call body also generates D traffic on the same loop iteration.
; ═══════════════════════════════════════════════════════════════
compute_fn:
    MOV  R9, R7                 ; echo input
    LDH  R11, [R5]              ; secondary D-load (same iter, same line)
    JMP  R13
compute_fn_end:
