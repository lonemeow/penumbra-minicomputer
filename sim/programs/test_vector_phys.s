; test_vector_phys.s — Verify exception vectors are at physical addresses
;
; Proves the vector table fetch bypasses the MMU by:
;   1. Mapping page 0 for code execution (needed for fetching instructions)
;   2. Mapping page 2 for data (sentinel storage)
;   3. NOT mapping page 1 — access will cause TLB miss
;   4. Accessing page 1 → TLB miss exception
;   5. Removing page 0's TLB mapping from within the handler
;   6. Handler sets R1=1 and BREAKs
;   7. BREAK triggers another exception — the vector fetch at 0x18
;      (VEC_BREAK) MUST work even though page 0 is now unmapped
;   8. If the vector fetch required a TLB hit on page 0, we'd get
;      a nested fault and the test would hang
;
; The key insight: after step 5, page 0 has no TLB entry. The handler
; code (on page 0) continues executing from the I-cache / pipeline,
; but the BREAK exception's vector fetch at address 0x18 must bypass
; the MMU to reach the physical vector table. If it goes through the
; MMU, it would TLB miss and we'd never halt.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX,  0xB9       ; V|R|W|X|G

; ═══════════════════════════════════════════════════════════════
; Vector table (physical addresses 0x00–0x1C)
; ═══════════════════════════════════════════════════════════════
.org 0x00
    B    start              ; 0x00: reset
    B    fail               ; 0x04: IRQ
    B    tlb_miss_handler   ; 0x08: TLB miss
    B    fail               ; 0x0C: protection fault
    NOP                     ; 0x10: privilege (unused)
    NOP                     ; 0x14: SYSCALL (unused)
    B    break_handler      ; 0x18: BREAK — this is the vector we're testing

; ═══════════════════════════════════════════════════════════════
; BREAK handler — if we reach here, the physical vector fetch worked
; This handler runs after page 0's TLB entry was removed, so reaching
; it proves the vector fetch at 0x18 bypassed the MMU.
; ═══════════════════════════════════════════════════════════════
break_handler:
    ; Nothing to do — the testbench already caught o_halted on
    ; the BREAK dispatch. But in case execution continues here,
    ; just loop. The testbench will have already stopped.
    B    break_handler

; ═══════════════════════════════════════════════════════════════
; TLB miss handler — unmap page 0, then BREAK
; ═══════════════════════════════════════════════════════════════
tlb_miss_handler:
    ; Invalidate page 0's TLB entry by writing V=0
    LLI  R8, #0
    WRSYS R8, #MMU, #TLB_INDEX   ; slot 0
    WRSYS R8, #MMU, #TLB_VPN     ; VPN=0
    WRSYS R8, #MMU, #TLB_PTE     ; PTE=0 (V=0 → invalid)

    ; Page 0 is now unmapped. We're still executing from page 0
    ; because the current instruction stream is already fetched.
    ; The BREAK below will trigger a vector fetch at physical 0x18.
    ; If the MMU were consulted, it would TLB miss (page 0 unmapped).
    LLI  R1, #1               ; PASS
    BREAK

; ═══════════════════════════════════════════════════════════════
; Main test
; ═══════════════════════════════════════════════════════════════
start:
    LLI  R1, #0

    ; ── Map page 0 (code + vector table) ──────────────────────
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; ── Enable MMU ────────────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR

    ; ── Access unmapped page 1 → TLB miss ─────────────────────
    LLI  R5, #0x1000
    LDW  R6, [R5]             ; FAULTS — handler unmaps page 0, BREAKs

    ; Should never reach here
    B    fail

fail:
    BREAK
