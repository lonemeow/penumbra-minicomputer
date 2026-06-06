; penumbra2_syscall.s — gen2 software-trap test: SYSCALL raises at EX and
; vectors to its handler.
;
; SYSCALL is tagged a trap in decode; EX raises it as a fault (VEC_SYSCALL),
; which commits at WB and vectors through the handler installed in the RAM
; table. The handler proves it ran by setting the PASS flag. The poison
; between the SYSCALL and the handler must be flushed by the trap entry.
;
; The handler ends in BREAK (the testbench program-end sentinel) rather than
; ERET — this checks trap *entry*, not return. Self-checks into R1.

_start:
    LLI  R1, #0                 ; assume FAIL
    LA   R2, sys_handler
    LLI  R3, #0x14             ; VEC_SYSCALL (5) << 2
    STW  R2, [R3]              ; vector_table[VEC_SYSCALL] = handler

    SYSCALL                     ; trap → VEC_SYSCALL

    LLI  R1, #0                ; poison: must be flushed by the trap entry
    BREAK                      ; poison

sys_handler:
    LLI  R1, #1                ; PASS — the SYSCALL vectored here
    BREAK
