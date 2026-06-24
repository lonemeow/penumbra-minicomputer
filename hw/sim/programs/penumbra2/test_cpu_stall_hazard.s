; test_cpu_stall_hazard.s — gen2 STALL_HAZARD / STALL_FLUSH counters
; REQUIRES: mmu cache pinned-stalls
;
; Both counters only register with a cache-hot front end. A RAW hazard forms
; only when the consumer reaches ID while its producer is still in EX/MEM/WB —
; i.e. they are fetched within the pipeline depth of each other. With an
; uncached front end every fetch takes several cycles, so a producer always
; retires before its consumer issues and no hazard ever forms (those cycles are
; STALL_IFETCH instead). Likewise a branch's redirect penalty only shows as
; FLUSH when the target re-fetches from cache; a miss would charge IFETCH.
;
; So this test maps the ROM code page cacheable (PTE C bit), enables the MMU and
; I-cache, and runs a dependent-ALU loop. The first iteration fills the I-cache;
; the hot iterations fetch back-to-back, so the dependent ADD chain RAW-stalls
; (HAZARD) and each taken loop branch flushes the front end (FLUSH).
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD     ; identity-map the ROM page, cacheable (V|C|R|W|X|G)

_start:
    LLI  R1, #0                 ; assume FAIL

    ; ── Map the ROM code page cacheable, enable MMU + I-cache ──
    LLI  R2, #16                ; conventional ROM-page TLB slot
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR          ; enable MMU
    WRSYS R4, #ICACHE, #CACHE_CTRL  ; enable I-cache

    ; ── Dependent-ALU loop: pass 1 warms the cache, hot passes
    ;    RAW-stall on the R5 chain and flush on each taken branch. ──
    LLI  R5, #0
    LLI  R6, #1
    LLI  R7, #8                     ; iterations
    RDSYS R10, #CPU, #STALL_HAZARD  ; before
    RDSYS R12, #CPU, #STALL_FLUSH
loop:
    ADD  R5, R6                     ; each ADD reads R5 from the previous one
    ADD  R5, R6
    ADD  R5, R6
    ADD  R5, R6
    SUB  R7, #1
    BNE  loop
    RDSYS R11, #CPU, #STALL_HAZARD  ; after
    RDSYS R13, #CPU, #STALL_FLUSH

    SUB  R11, R10                   ; HAZARD delta
    CMP  R11, R0
    BEQ  fail                       ; no hazard cycles → FAIL
    SUB  R13, R12                   ; FLUSH delta
    CMP  R13, R0
    BEQ  fail                       ; no flush cycles → FAIL

    LLI  R1, #1                     ; both counters advanced → PASS
fail:
    BREAK
