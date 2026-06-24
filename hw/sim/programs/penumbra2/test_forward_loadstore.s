; test_forward_loadstore.s — load/store operand forwarding, cache-hot.
; REQUIRES: mmu cache
;
; The memory-operand forward paths real code (Dhrystone prologues, struct copies)
; leans on, all at d=1 in a warm loop so the producer/consumer pair is back-to-
; back and actually forwards:
;   - store DATA forwarded   (ALU result → STW datum);
;   - store BASE forwarded    (ALU result → STW base address);
;   - load-use                (LDW result → ALU consumer, the 1-cycle interlock);
;   - load-use into a load BASE (LDW a pointer → LDW through it).
; Each is checked by value / round-trip through an independently-computed address,
; so a stale forward shows up as a mismatch.
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ ROM_PTE_C, 0xFFFF00BD      ; ROM code page, cacheable

_start:
    LLI  R1, #0

    ; ── Map data page 1 (VA 0x1000 → PPN 1, cacheable) + ROM code page ──
    LLI  R2, #1
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R2, #0x0100                ; VPN=1
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R2, #0x10BD                ; PPN=1, V|C|R|W|X|G
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR
    WRSYS R4, #ICACHE, #CACHE_CTRL
    WRSYS R4, #DCACHE, #CACHE_CTRL

    ; Seed: a pointer at 0x1020 → 0x1040, and the value 0xBEEF at 0x1040.
    LLI  R2, #0x1040
    LLI  R3, #0x1020
    STW  R2, [R3]                   ; mem[0x1020] = 0x1040
    LLI  R2, #0xBEEF
    LLI  R3, #0x1040
    STW  R2, [R3]                   ; mem[0x1040] = 0xBEEF

    LLI  R5, #0x1004                ; fixed round-trip slot
    LLI  R6, #4
    LLI  R7, #16
    LLI  R12, #0                    ; checksum
loop:
    ; ── store-DATA forward d1 + load-use d1 round-trip ──────────────
    ADD  R8, R6                     ; (P) R8 = 4,8,...,64 — value producer
    STW  R8, [R5]                   ; store-DATA forward d1 (datum R8 just produced)
    LDW  R9, [R5]
    ADD  R12, R9                    ; load-use d1 (R9 from load → ALU)

    ; ── store-BASE forward d1 ───────────────────────────────────────
    LLI  R10, #0x1080
    ADD  R10, R8                    ; (P) base = 0x1080+R8 (0x1084..0x10C0, word-aligned)
    STW  R6, [R10]                  ; store-BASE forward d1 (base R10 just produced)
    LLI  R11, #0x1080
    ADD  R11, R8                    ; independent recompute of the same address
    LDW  R4, [R11]
    CMP  R4, R6                     ; must equal what we stored (4)
    BNE  fail

    ; ── load-use into a load BASE (LDW ptr → LDW [ptr]) ─────────────
    LLI  R3, #0x1020
    LDW  R2, [R3]                   ; R2 = *0x1020 = 0x1040 (the pointer)
    LDW  R4, [R2]                   ; load-base load-use d1: R4 = *0x1040 = 0xBEEF
    CMP  R4, #0xBEEF
    BNE  fail

    SUB  R7, #1
    BNE  loop

    CMP  R12, #544                  ; 4+8+...+64
    BNE  fail
    LLI  R1, #1
fail:
    BREAK
