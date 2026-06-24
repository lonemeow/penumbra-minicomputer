; test_forward_spr.s — SPR / sysreg operand paths under forwarding, cache-hot.
; REQUIRES: mmu cache wrspr
;
; The physical-register namespace beyond the GPRs, exercised back-to-back in a
; warm loop:
;   - USP (entry 14) is regfile-backed → WRSPR-USP forwards to a following
;     RDSPR-USP exactly like a GPR;
;   - SCRn is scratch-file-backed → forwarding is deferred, so WRSPR-SCR0 then
;     RDSPR-SCR0 takes the conservative stall but still reads the right value;
;   - RDSYS produces its value in MEM (load-like), so a consumer right behind it
;     takes the load-use interlock and forwards from MEM/WB.
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD      ; ROM code page, cacheable

_start:
    LLI  R1, #0

    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #ICACHE, #CACHE_CTRL

    LLI  R10, #16
loop:
    ; ── USP forward (regfile-backed SPR) ────────────────────────────
    LLI  R2, #0x55
    WRSPR USP, R2                   ; USP ← 0x55
    RDSPR R3, USP                   ; read back — forwards d1 like a GPR
    CMP  R3, #0x55
    BNE  fail

    ; ── SCRn conservative (scratch-file, forwarding deferred) ───────
    LLI  R2, #0xAB
    WRSPR SCR0, R2                  ; SCR0 ← 0xAB
    RDSPR R3, SCR0                  ; read back — conservative stall, correct value
    CMP  R3, #0xAB
    BNE  fail

    ; ── RDSYS-use (load-like producer) ──────────────────────────────
    LLI  R2, #0x77
    WRSYS R2, #6, #0               ; scratch device 6, reg 0 ← 0x77
    RDSYS R4, #6, #0               ; R4 ← 0x77 (produced in MEM)
    MOV  R8, R4                     ; consume right behind it — load-use, forwards from MEM/WB
    CMP  R8, #0x77
    BNE  fail

    SUB  R10, #1
    BNE  loop

    LLI  R1, #1
fail:
    BREAK
