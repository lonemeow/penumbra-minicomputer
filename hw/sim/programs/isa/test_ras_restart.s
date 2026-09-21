; test_ras_restart.s — a restartable atomic sequence under interrupt load
; REQUIRES: timer irq
;
; A load-compare-store sequence is atomic against an interrupt handler
; on the same CPU only if a trap taken inside it rewinds PC to the start
; instead of resuming.  Resuming commits a value computed before the
; handler ran, silently losing whatever the handler wrote.  Both the
; libc CAS and the kernel's restart path depend on this property, so it
; is pinned here rather than inferred.
;
; A timer handler contends for the same word the sequence increments,
; and runs throughout both phases:
;
;   phase 1, restart on:   W == N + M — a restart recovers every
;                          collision, so nothing is lost
;   phase 2, restart off:  W == N + M - INSIDE — a collision strictly
;                          inside the region loses exactly the one
;                          increment the handler made, no more
;
; Phase 2 is the control: without it, a passing phase 1 could equally
; mean no interrupt ever landed in the window.  Checking it for an exact
; count rather than "less than expected" makes it a measurement of the
; race rather than a smoke test.
;
; What the phases need is a timer period expressed in iterations, and
; nothing declares that: TMFREQ gives the tick rate, but the ticks an
; iteration costs also depends on how many cycles it takes and on the
; prescaler ahead of the counter, neither of which is promised.  So the
; period is measured against the counter itself — the one unit reload is
; expressed in on any implementation — and the sweep derived from that.
; A period below the handler's own cost cannot be escaped, since every
; retry is interrupted again; one far above it leaves a sweep value with
; no arrivals at all.  Stepping by a tick walks the arrival point
; through the sequence, where a single period lands on the same offsets
; forever.
;
; Result: R1 = 1 PASS, R1 = 0 FAIL.

.equ W_ADDR,        0x0040        ; contended word (vector table is 0x00..0x2C)
.equ M_ADDR,        0x0044        ; handler invocations
.equ REN_ADDR,      0x0048        ; restart enable
.equ LR_SAVE,       0x004C        ; link register across a nested call
.equ HIST_BASE,     0x0050        ; five buckets, indexed by byte offset
.equ BASE_ADDR,     0x0064        ; measured period

.equ RAS_BYTES,     16            ; the region: load, compare, branch, store
.equ N_INNER,       200           ; increments per period, and per measurement
.equ N_SWEEP,       16            ; consecutive periods
.equ N_TOTAL,       3200          ; N_INNER * N_SWEEP
.equ ITERS_PER_IRQ, 8             ; target spacing, in iterations
.equ RELOAD_FLOOR,  8             ; below this the handler cannot be escaped
.equ CAL_RELOAD,    0x00FFFFFF    ; long enough not to wrap while measuring

_start:
    LLI  R1, #0                   ; FAIL unless every check below passes

    LA   R2, #timer_handler
    STW  R2, [R0 + #4]            ; vector 1 = timer

    BL   calibrate

    ; ── Phase 1: restart enabled ─────────────────────────────────
    LLI  R2, #1
    STW  R2, [R0 + #REN_ADDR]
    BL   run_phase                ; → R5 = W, R6 = M, R7 = INSIDE
    BL   check_coverage

    LI   R10, #N_TOTAL
    MOV  R9, R6
    ADD  R9, R10                  ; R9 = N + M
    CMP  R5, R9
    BNE  done                     ; a restart must lose nothing

    ; ── Phase 2: restart disabled ────────────────────────────────
    STW  R0, [R0 + #REN_ADDR]
    BL   run_phase
    BL   check_coverage

    LI   R10, #N_TOTAL
    MOV  R9, R6
    ADD  R9, R10                  ; R9 = N + M
    SUB  R9, R7                   ; R9 = N + M - INSIDE
    CMP  R5, R9
    BNE  done                     ; each collision must lose exactly one

    LLI  R1, #1                   ; PASS
done:
    BREAK

; ═══════════════════════════════════════════════════════════════
; calibrate — time one batch against the timer's own counter and pick
; a period from it, so the sweep means the same thing wherever the
; tick rate, the prescaler and the cycles per instruction differ.
; ═══════════════════════════════════════════════════════════════
calibrate:
    STW  R13, [R0 + #LR_SAVE]

    STW  R0, [R0 + #W_ADDR]
    LLI  R8, #W_ADDR

    LI   R3, #CAL_RELOAD
    WRSYS R3, #TIMER, #TM_RELOAD
    WRSYS R3, #TIMER, #TM_COUNT
    LLI  R3, #0x01                ; TICK_EN, no interrupt
    WRSYS R3, #TIMER, #TM_CR

    RDSYS R6, #TIMER, #TM_COUNT
    LLI  R5, #0                   ; run_batch leaves the timer alone
    BL   run_batch
    RDSYS R7, #TIMER, #TM_COUNT

    LLI  R3, #0
    WRSYS R3, #TIMER, #TM_CR

    SUB  R6, R7                   ; ticks the batch consumed
    LLI  R7, #ITERS_PER_IRQ
    MUL  R6, R7
    LI   R7, #N_INNER
    DIVU R6, R7                   ; ticks per ITERS_PER_IRQ iterations

    LLI  R7, #RELOAD_FLOOR
    CMP  R6, R7
    BHS  cal_done
    MOV  R6, R7                   ; too fast to escape → clamp
cal_done:
    STW  R6, [R0 + #BASE_ADDR]

    LDW  R13, [R0 + #LR_SAVE]
    RET

; ═══════════════════════════════════════════════════════════════
; check_coverage — every landing point that can distinguish a correct
; bound from a wrong one must have been reached.  Falls through to
; `done` with R1 still 0 on a miss, which is the failure exit.
; ═══════════════════════════════════════════════════════════════
check_coverage:
    LLI  R9, #4                   ; offset 0 is excluded deliberately: with
                                  ; nothing of the sequence yet run, rewinding
                                  ; to the start and resuming are the same
                                  ; act, so an arrival there cannot tell a
                                  ; correct lower bound from a wrong one.
                                  ; The bucket is still counted, just not
                                  ; required.
cov_loop:
    LDW  R10, [R9 + #HIST_BASE]
    CMP  R10, R0
    BEQ  done                     ; never landed here → coverage incomplete
    ADD  R9, #4
    LLI  R10, #RAS_BYTES
    CMP  R9, R10
    BLS  cov_loop
    RET

; ═══════════════════════════════════════════════════════════════
; run_phase — sweep the period upward from the measured base and
; return the totals.
; ═══════════════════════════════════════════════════════════════
run_phase:
    STW  R13, [R0 + #LR_SAVE]

    STW  R0, [R0 + #W_ADDR]
    STW  R0, [R0 + #M_ADDR]
    LLI  R9, #0
hz_loop:
    STW  R0, [R9 + #HIST_BASE]
    ADD  R9, #4
    LLI  R10, #RAS_BYTES
    CMP  R9, R10
    BLS  hz_loop

    LLI  R8, #W_ADDR
    LDW  R5, [R0 + #BASE_ADDR]
    LLI  R6, #N_SWEEP
sweep:
    BL   run_batch
    ADD  R5, #1                   ; a tick's step walks the arrival point
    SUB  R6, #1
    CMP  R6, R0
    BNE  sweep

    LDW  R5, [R0 + #W_ADDR]
    LDW  R6, [R0 + #M_ADDR]

    LLI  R7, #0                   ; INSIDE = the three interior buckets
    LLI  R9, #4
in_loop:
    LDW  R10, [R9 + #HIST_BASE]
    ADD  R7, R10
    ADD  R9, #4
    LLI  R10, #12
    CMP  R9, R10
    BLS  in_loop

    LDW  R13, [R0 + #LR_SAVE]
    RET

; ═══════════════════════════════════════════════════════════════
; run_batch — N_INNER increments.  R5 = timer period, or 0 to leave
; the timer as the caller set it.  R8 = &W.  The sequence is inlined
; so that no call inside it clobbers a return address.
;
; Live across the region: R2 (expected), R3 (new), R8, R11 (loaded),
; R4 (counter), R5 (period).  The handler touches none of them.
; ═══════════════════════════════════════════════════════════════
run_batch:
    CMP  R5, R0
    BEQ  rb_start
    WRSYS R5, #TIMER, #TM_RELOAD
    WRSYS R5, #TIMER, #TM_COUNT
    LLI  R3, #0x07                ; TICK_EN | IRQ_EN | AUTOLOAD
    WRSYS R3, #TIMER, #TM_CR
    EI
rb_start:
    LI   R4, #N_INNER
rb_next:
    LDW  R2, [R8]                 ; expected
    MOV  R3, R2
    ADD  R3, #1                   ; new

    ; The region proper.  The store is its last instruction, so a trap
    ; at ras_end means the sequence finished — by storing, or by the
    ; compare branching past it.
ras_start:
    LDW  R11, [R8]
    CMP  R11, R2
    BNE  ras_end
    STW  R3, [R8]
ras_end:

    CMP  R11, R2
    BNE  rb_next                  ; lost the race → retry with fresh values
    SUB  R4, #1
    CMP  R4, R0
    BNE  rb_next

    CMP  R5, R0
    BEQ  rb_done
    LLI  R3, #0
    WRSYS R3, #TIMER, #TM_CR
    DI
rb_done:
    RET

; ═══════════════════════════════════════════════════════════════
; Timer handler — contend for W, bucket the arrival by its offset into
; the region, and rewind if enabled.  Uses only R9, R10 and R12, none
; of which the loop keeps live.  ERET restores SR, so the compare flags
; survive an entry taken between the compare and its branch.
; ═══════════════════════════════════════════════════════════════
timer_handler:
    LDW  R9, [R0 + #W_ADDR]
    ADD  R9, #1
    STW  R9, [R0 + #W_ADDR]       ; the colliding write

    LDW  R9, [R0 + #M_ADDR]
    ADD  R9, #1
    STW  R9, [R0 + #M_ADDR]

    RDSPR R9, EPC
    LA   R10, #ras_start
    SUB  R9, R10                  ; byte offset into the region
    LLI  R12, #RAS_BYTES
    CMP  R9, R12
    BHI  h_ack                    ; outside the region and its end marker

    LDW  R12, [R9 + #HIST_BASE]
    ADD  R12, #1
    STW  R12, [R9 + #HIST_BASE]

    CMP  R9, R0
    BEQ  h_ack                    ; at the start → nothing begun
    LLI  R12, #RAS_BYTES
    CMP  R9, R12
    BEQ  h_ack                    ; at the end → already finished

    LDW  R12, [R0 + #REN_ADDR]
    CMP  R12, R0
    BEQ  h_ack
    WRSPR EPC, R10                ; rewind to the start of the sequence

h_ack:
    LLI  R10, #1
    WRSYS R10, #TIMER, #TM_STATUS
    ERET
