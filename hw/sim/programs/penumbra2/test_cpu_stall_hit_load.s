; test_cpu_stall_hit_load.s — gen2 cache-hit load attribution (SYSDEV_CPU STALL_LOAD)
; REQUIRES: mmu cache
;
; Regression for the perfctr stall-attribution fix. A D-cache *hit* load pays a
; single MEM launch-cycle stall, and the bubble it injects reaches the commit
; point the cycle after — by which time the load's stall signal has cleared. The
; old live-signal attribution sampled cause and effect in the same cycle, so that
; orphaned cycle fell into the front-end residual (STALL_FLUSH): a run of
; cache-hit loads scored ~zero in STALL_LOAD, the whole reason the bug hid behind
; "flush." With carried-cause attribution each hit-load's bubble is tagged LOAD
; where it is born and charged there, so a burst of N hits moves STALL_LOAD by at
; least N and does not leak into STALL_FLUSH.
;
; Reading the counters is itself an RDSYS — a sysreg access sharing the MEM access
; FSM — and RDSYS is charged to the residual, never LOAD/STORE, so the LOAD delta
; measured across the RDSYS snapshots is the burst's alone (the instrument stays
; out of the bucket it measures).
;
; Result: R1=1 PASS, R1=0 FAIL; run via tb_penumbra2_prog.

.equ KERN_RWX_C, 0xBD          ; V|C|R|W|X|G — cacheable data page (PPN 0)
.equ ROM_PTE_C,  0xFFFF00BD    ; cacheable ROM code page
.equ TEST_ADDR,  0x0800        ; within cacheable page 0
.equ TEST_VAL,   0x1234

_start:
    LLI  R1, #0                ; assume FAIL

    ; ── Map data page 0 + ROM page cacheable; enable MMU + caches ──
    LLI  R2, #0
    WRSYS R2, #MMU, #TLB_INDEX
    WRSYS R2, #MMU, #TLB_VPN        ; VPN 0 → PPN 0
    LLI  R3, #KERN_RWX_C
    WRSYS R3, #MMU, #TLB_PTE        ; cacheable data page

    LLI  R2, #16
    WRSYS R2, #MMU, #TLB_INDEX
    LI   R2, #0x0FFFF000
    WRSYS R2, #MMU, #TLB_VPN
    LI   R2, #ROM_PTE_C
    WRSYS R2, #MMU, #TLB_PTE        ; cacheable ROM code page

    LLI  R4, #1
    WRSYS R4, #MMU, #MMUCR          ; enable MMU
    WRSYS R4, #DCACHE, #CACHE_CTRL  ; enable D-cache
    WRSYS R4, #ICACHE, #CACHE_CTRL  ; enable I-cache

    ; ── Warm the line so the measured burst all hits ──
    LLI  R5, #TEST_VAL
    LLI  R6, #TEST_ADDR
    STW  R5, [R6]                   ; write-through, no allocate
    LDW  R7, [R6]                   ; cold read miss → line fill → cached
    LDW  R7, [R6]                   ; settle: now a hit

    ; ── Snapshot, then 8 cache-hit loads ──
    RDSYS R10, #CPU, #STALL_LOAD    ; before  (RDSYS → residual, leaves LOAD clean)
    RDSYS R12, #CPU, #STALL_FLUSH
    LDW  R7, [R6]
    LDW  R7, [R6]
    LDW  R7, [R6]
    LDW  R7, [R6]
    LDW  R7, [R6]
    LDW  R7, [R6]
    LDW  R7, [R6]
    LDW  R7, [R6]
    RDSYS R11, #CPU, #STALL_LOAD    ; after
    RDSYS R13, #CPU, #STALL_FLUSH

    ; ── The 8 hits must move STALL_LOAD by at least 8 ──
    SUB  R11, R10                   ; STALL_LOAD delta
    CMPI R11, #8
    BLT  fail                       ; hit-loads not charged to LOAD → the old bug

    ; ── ...and must not leak into the front-end residual ──
    SUB  R13, R12                   ; STALL_FLUSH delta (only the RDSYS snapshots)
    CMP  R11, R13
    BLE  fail                       ; LOAD must dominate — attribution didn't flip

    LLI  R1, #1                     ; PASS
fail:
    BREAK
