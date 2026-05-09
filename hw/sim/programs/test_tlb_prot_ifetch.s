; test_tlb_prot_ifetch.s — Fetch protection fault on a CACHEABLE page
;
; Why this test exists
; ────────────────────
; Regression sentinel for the cached fetch-protection-fault path.
; Without proper gating of `icache.i_re` against `mmu_fault`, a fetch
; into a page mapped X=0, C=1 would hang: the cache lookup runs
; unconditionally, the tag mismatches so `o_busy=1`, but the fill
; state machine is gated against faulting accesses and never starts —
; sequencer parked in S_FETCH waiting for an `ir_valid` that never
; comes.
;
; This test sets up exactly that configuration:
;   • A page mapped R+W but NOT X, with PTE.C=1 (cacheable).
;   • I-cache master enable bit set (so PTE.C actually takes effect).
;   • Branch into that page in supervisor mode.
;
; Pass criterion: handler runs, fault is delivered cleanly, retry
; after re-mapping with X works.  Failure criterion: testbench
; cycle limit (500000 cycles) hits before BREAK — that's a hang.
;
; Result: R1=1 PASS, R1=0 FAIL

.equ KERN_RWX,     0xB9     ; V|R|W|X|G  — uncached (used for slot-0 PPN=0)
.equ PTE_RW_C_P2,  0x209D   ; PPN=2, V|C|R|W|G — cacheable, NO X (the trap setup)
.equ PTE_RWX_C_P2, 0x20BD   ; PPN=2, V|C|R|W|X|G — full access cacheable
.equ TARGET_ADDR,  0x2000   ; page 2 — the X=0, C=1 page

.equ FAULT_PROT,  2        ; FAULT_STATUS[3:0]
.equ FSTAT_X,     0x400    ; bit [10] = execute access

_start:
    LLI  R1, #0              ; assume fail

    ; Install protection fault handler at vector slot 3 (offset 0x0C)
    LA   R2, #ifetch_prot_handler
    STW  R2, [R0 + #0x0C]

    ; Copy target code from ROM to RAM at TARGET_ADDR (MMU still off)
    LA   R2, #target_code
    LLI  R3, #TARGET_ADDR
    LA   R4, #target_code_end
copy:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy

    ; Slot 0: VPN 0 → PPN 0 (vectors + handler scratch), uncached RWX
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN
    LLI  R3, #KERN_RWX
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 2: VPN 2 → PPN 2, R+W only, CACHEABLE — the trap page
    LLI  R2, #2
    WRSYS R2, #MMU, #TLB_INDEX
    LLI  R3, #0x0200          ; VPN=2, ASID=0
    WRSYS R3, #MMU, #TLB_VPN
    LLI  R3, #PTE_RW_C_P2      ; PPN=2, V|C|R|W|G  (NO X)
    WRSYS R3, #MMU, #TLB_PTE

    ; Slot 16: ROM page (so we keep fetching while MMU is on)
    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R3, #0x0FFFF000
    WRSYS R3, #MMU, #TLB_VPN
    LI   R3, #0xFFFF00B9
    WRSYS R3, #MMU, #TLB_PTE

    ; Enable MMU + I-cache (PTE.C alone isn't enough — cache master
    ; enable bit must be set for the cached fetch path to engage)
    LLI  R5, #1
    WRSYS R5, #MMU, #MMUCR
    WRSYS R5, #ICACHE, #CACHE_CTRL

    ; Jump into the no-X, cacheable page → expect fetch prot fault
    LI   R6, #TARGET_ADDR
    JMP  R6

    ; If we reach here, the fault didn't fire (unexpected fall-through)
fail:
    BREAK

; ═══════════════════════════════════════════════════════════════
; Fetch protection fault handler
; ═══════════════════════════════════════════════════════════════
ifetch_prot_handler:
    ; FAULT_ADDR should be the target instruction address
    RDSYS R8, #MMU, #FAULT_ADDR
    LI    R9, #TARGET_ADDR
    CMP   R8, R9
    BNE   fail

    ; FAULT_STATUS bits[3:0] should be FAULT_PROT
    RDSYS R8, #MMU, #FAULT_STATUS
    LLI   R9, #0x0F
    MOV   R13, R8
    AND   R13, R9
    CMP   R13, #FAULT_PROT
    BNE   fail

    ; FAULT_STATUS bit [10] (X access) should be set
    MOV   R13, R8
    LI    R9, #FSTAT_X
    AND   R13, R9
    CMP   R13, R9
    BNE   fail

    ; Remap VPN 2 → PPN 2 with X enabled (full RWX cacheable)
    LLI   R8, #2
    WRSYS R8, #MMU, #TLB_INDEX
    LLI   R8, #0x0200
    WRSYS R8, #MMU, #TLB_VPN
    LLI   R8, #PTE_RWX_C_P2    ; PPN=2 + V|C|R|W|X|G — grant X
    WRSYS R8, #MMU, #TLB_PTE

    ; Retry the faulting fetch
    ERET

; ═══════════════════════════════════════════════════════════════
; Target code — runs from 0x2000 after the handler grants X
; ═══════════════════════════════════════════════════════════════
target_code:
    LLI  R1, #1
    BREAK
target_code_end:
