; test_tlb_resync.s — WRSYS context-sync re-derives the fetch under new state
; REQUIRES: mmu-d mmu-i
;
; The positive WRSYS-resynchronization test: a WRSYS that *unmaps the page
; holding its own successor* must cause that successor's fetch to take a
; TLB miss — observably proving the instructions after a WRSYS are fetched
; under the post-commit translation, never from stale fetch-ahead state
; (the context-synchronization contract in sysregs.md).
;
; Flow:
;   1. Copy the position-independent miss handler to RAM 0x0100 — inside
;      the one mapped RAM page (page 0), clear of the vector table — and
;      point vector[2] at it. All handler operands are passed in
;      registers so the copied code needs no absolute references.
;   2. Map page 0 (identity RWX) and the ROM code page; enable the MMU.
;   3. Invalidate the ROM code page's TLB entry — the instructions doing
;      the invalidation live on that very page. The successor of the
;      committing WRSYS (expect_fault) must take a fetch-side TLB miss.
;   4. The RAM handler verifies FAULT_ADDR = expect_fault and
;      FAULT_STATUS = {EXEC, FAULT_TLB_MISS} = 0x401, remaps the ROM
;      page, sets the came-through flag, and ERETs to retry.
;   5. The retried successor sees the flag set and passes.
;
; Register convention (handler operands set up before the fault):
;   R1  = pass/fail            R8  = handler came-through flag
;   R5  = ROM VPN word         R6  = ROM PTE word (valid, for the remap)
;   R10 = expected FAULT_ADDR  R12 = ROM page TLB index
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX, 0xB9          ; V|R|W|X|G

_start:
    LLI  R1, #0                ; assume fail
    LLI  R8, #0                ; came-through flag

    ; ── Copy the handler to RAM 0x0100; vector[2] → it ───────
    ; 0x0100 sits inside page 0, which gets the identity RWX mapping
    ; below — the handler must be fetchable while the ROM page is dead,
    ; or the miss recurses on the handler's own fetch.
    LA   R2, #miss_handler
    LA   R4, #miss_handler_end
    LLI  R3, #0x0100
copy_loop:
    LDW  R9, [R2]
    STW  R9, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy_loop
    LLI  R2, #0x0100
    STW  R2, [R0 + #8]         ; vector[2] = RAM handler

    ; ── Map page 0 (identity RWX: vectors, handler, data) ────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Map the ROM code page; keep its words for the remap ──
    LLI  R12, #16
    WRSYS R12, #MMU, #TLB_INDEX
    LI   R5, #0x0FFFF000       ; VPN word: VPN=0xFFFF0, ASID=0
    WRSYS R5, #MMU, #TLB_VPN
    LI   R6, #0xFFFF00B9       ; PTE word: PPN=0xFFFF0, KERN_RWX
    WRSYS R6, #MMU, #TLB_PTE

    ; ── Enable the MMU (fetches translate from here on) ──────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── The probe: unmap the page these instructions live on ─
    LA   R10, #expect_fault    ; the successor the handler must see fault

    ; TODO(human): invalidate the ROM code page's TLB entry.

    WRSYS R12, #MMU, #TLB_INDEX
    WRSYS R5,  #MMU, #TLB_VPN
    WRSYS R0,  #MMU, #TLB_PTE

expect_fault:
    ; Reached with R8=0 only if the successor executed under the dead
    ; translation — the resynchronization failed. The handler's ERET
    ; retry arrives here with R8=1.
    CMP  R8, #1
    BNE  fail
    LLI  R1, #1
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Miss handler — copied to RAM, so it must be position-independent:
; local (relative) branches only, all operands from registers.
; ═══════════════════════════════════════════════════════════════
miss_handler:
    RDSYS R9, #MMU, #FAULT_ADDR
    CMP  R9, R10               ; faulted exactly at the WRSYS successor?
    BNE  handler_bad
    RDSYS R9, #MMU, #FAULT_STATUS
    LI   R11, #0x401           ; {user=0, ACC_EXEC, FAULT_TLB_MISS}
    CMP  R9, R11
    BNE  handler_bad
    ; Remap the ROM page from the register-passed entry words.
    WRSYS R12, #MMU, #TLB_INDEX
    WRSYS R5, #MMU, #TLB_VPN
    WRSYS R6, #MMU, #TLB_PTE
    LLI  R8, #1                ; came through the fault
    ERET                       ; retry expect_fault under the remap
handler_bad:
    LLI  R1, #0
    BREAK
miss_handler_end:
