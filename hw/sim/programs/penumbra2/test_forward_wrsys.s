; test_forward_wrsys.s — a WRSYS whose value register was just computed (hot).
; REQUIRES: mmu cache
;
; WRSYS composes its sysreg write datum in the spine from the registered ID/EX
; operand (o_sys_wdata = idex_op_b), which bypasses the EX operand-forwarding
; network. So a WRSYS value register cannot be forwarded: if the variant relaxes
; its issue interlock on the strength of forwarding, the WRSYS writes the stale
; pre-producer value. This is the bug that hung the TLB miss handler — it WRSYS-
; installs a freshly-computed PTE in hot, back-to-back code, so the value must
; be forwarded or, failing that, interlocked.
;
; To make the producer/WRSYS pair back-to-back (so a relaxed variant would have
; forwarded), the code page is cacheable and the pair runs in a warm loop. Each
; pass computes a fresh value, WRSYS-writes it to the scratch device, and reads
; it back: a stale write shows up as a mismatch (or, on the real handler, a
; re-fault loop).
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD     ; identity-map the ROM page, cacheable (V|C|R|W|X|G)

_start:
    LLI  R1, #0                 ; assume FAIL

    ; ── Map the ROM code page cacheable, enable MMU + I-cache ──
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #ICACHE, #CACHE_CTRL

    LLI  R5, #0x1000            ; running value base
    LLI  R6, #0x10
    LLI  R7, #16                ; iterations (>1 so the loop goes cache-hot)
loop:
    ADD  R5, R6                 ; (P) freshly compute the value...
    WRSYS R5, #6, #0           ; (C) ...and WRSYS it immediately: idex_op_b must
                                ;     be the computed R5, not the stale prior one
    RDSYS R8, #6, #0           ; read the scratch register back
    CMP  R8, R5                 ; must equal what we just wrote
    BNE  fail
    SUB  R7, #1
    BNE  loop

    LLI  R1, #1                 ; every hot WRSYS wrote the forwarded value → PASS
fail:
    BREAK
