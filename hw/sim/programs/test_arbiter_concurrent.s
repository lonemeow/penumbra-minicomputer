; test_arbiter_concurrent.s — fill-word-ordering sentinel for the
; cpu_bus_arbiter + cache_vipt + SDRAM stack
;
; Exercises the arbiter's owner-alternation paths: D-fills (data array
; loads) and I-fills (verify_func instruction fetches) alternate as the
; CPU sequencer hands off between fetch and execute.  Each iteration
; invalidates both caches first, so every call cold-fills both caches
; from SDRAM through the same arbiter handshake the kernel uses at
; speed.
;
; Strategy:
;   1. Map page 0 cacheable (SDRAM-backed); ROM page uncacheable.
;   2. Initialize a 16-line data array at DATA_BASE with address-as-data
;      (A[i] = &A[i]).  Address-as-data makes any corruption diagnosable
;      from a single LDW: the diff between expected and got tells you
;      what kind of slot/byte misroute occurred (e.g. +4 = adjacent-word
;      swap in the fill, large negative = response routed from a totally
;      different transaction).
;   3. Copy verify_func from ROM into cacheable RAM, so fetching it
;      after icache invalidate goes through the SDRAM I-fill path.
;   4. Each iteration:
;        - invalidate both caches;
;        - JMP into the RAM copy of verify_func;
;        - on first wrong load, verify_func returns R1=0 → BREAK.
;   5. After ITERATIONS clean passes, R1=1 → BREAK PASS.
;
; Result: R1=1 PASS, R1=0 FAIL.

.equ KERN_RWX_C, 0xBD              ; V|C|R|W|X|G — cacheable kernel page
.equ KERN_RWX,   0xB9              ; V|R|W|X|G   — uncacheable

; Memory layout in page 0 (cacheable identity-mapped):
;   0x0100..0x01FF: data array A[0..63] (16 cache lines of 4 words)
;   0x0200..       : RAM copy of verify_func
.equ DATA_BASE,  0x0100
.equ DATA_END,   0x0200            ; 64 words = 16 cache lines
.equ FUNC_DEST,  0x0200            ; RAM copy of verify_func

.equ ITERATIONS, 16

_start:
    LLI  R1, #0                    ; assume fail until proven otherwise

    ; ── Map page 0 cacheable, ROM uncacheable, enable MMU ─────
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

    ; ── Initialize data array: A[i] = its own address ─────────
    ; Done via the bypass path while caches are disabled — we want
    ; SDRAM to actually hold these values, so that the very first
    ; cached LDW after enable+invalidate hits SDRAM (cold miss).
    LLI  R2, #DATA_BASE
    LLI  R3, #DATA_END
init_data:
    STW  R2, [R2]                  ; *p = p
    ADD  R2, #4
    CMP  R2, R3
    BNE  init_data

    ; ── Copy verify_func from ROM into cacheable RAM ──────────
    LA   R2, #verify_func          ; ROM source
    LLI  R3, #FUNC_DEST            ; RAM destination
    LA   R4, #verify_func_end
copy_func:
    LDW  R5, [R2]
    STW  R5, [R3]
    ADD  R2, #4
    ADD  R3, #4
    CMP  R2, R4
    BNE  copy_func

    ; ── Enable both caches ────────────────────────────────────
    LLI  R4, #1
    WRSYS R4, #DCACHE, #CACHE_CTRL
    WRSYS R4, #ICACHE, #CACHE_CTRL

    ; ── Outer iteration loop ──────────────────────────────────
    LLI  R10, #ITERATIONS

iter_loop:
    ; Invalidate both caches.  After this, the next call into
    ; verify_func cold-fills both caches concurrently from SDRAM:
    ; icache fills as we sequence through the function body, and
    ; dcache fills on each LDW at a fresh A[i] line.
    WRSYS R0, #DCACHE, #CACHE_INVAL_ALL
    WRSYS R0, #ICACHE, #CACHE_INVAL_ALL

    ; Call the RAM copy of verify_func.  R13 = return address.
    LLI  R11, #FUNC_DEST
    LA   R13, #after_verify
    JMP  R11

after_verify:
    CMP  R1, #1                    ; verify_func returns R1=0 on mismatch
    BNE  done                      ; bail out on first failure

    SUB  R10, #1
    CMP  R10, R0
    BNE  iter_loop

    ; All iterations clean.
    LLI  R1, #1
done:
    BREAK


; ═══════════════════════════════════════════════════════════════
; verify_func — load every word in DATA_BASE..DATA_END and verify
; address-as-data pattern.  Lives in ROM; the test copies it into
; RAM before calling so icache fills hit the SDRAM path under test.
;
; Inputs:    R13 = return address (set by caller)
; Output:    R1 = 1 (all words match), R1 = 0 (first mismatch hit)
; Clobbers:  R5–R9 (caller-saved equivalents in this test)
; ═══════════════════════════════════════════════════════════════
verify_func:
    LLI  R5, #DATA_BASE            ; cursor — also the expected value at [R5]
    LLI  R6, #DATA_END             ; sentinel

verify_loop:
    ldw r7, [r5]
    cmp r5, r7
    bne verify_fail
    inc r5, #4
    cmp r5, r6
    bne verify_loop

verify_pass:
    LLI  R1, #1
    JMP  R13

verify_fail:
    LLI  R1, #0
    JMP  R13
verify_func_end:
