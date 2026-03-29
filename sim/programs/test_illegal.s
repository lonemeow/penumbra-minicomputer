; test_illegal.s — test illegal instruction trap
;
; Verifies:
;   1. Executing an undefined instruction traps to vector 7 (VEC_ILLEGAL)
;   2. Handler can skip the faulting instruction and resume execution
;   3. Multiple different illegal instructions all trap correctly

; ── Vector table ─────────────────────────────────────────
.org 0x00
    B start                     ; 0x00: reset
    .word 0                     ; 0x04: IRQ (unused)
    .word 0                     ; 0x08: TLB miss (unused)
    .word 0                     ; 0x0C: TLB prot (unused)
    .word 0                     ; 0x10: priv (unused)
    .word 0                     ; 0x14: syscall (unused)
    .word 0                     ; 0x18: break (unused)
    B illegal_handler           ; 0x1C: illegal instruction (vector 7)

; ── Illegal instruction handler ──────────────────────────
;
; Must skip the faulting instruction and resume execution.
; Note: RTI would return to EPC (the faulting instruction),
; causing an infinite trap loop. Use IRET instead, which lets
; you specify an arbitrary return address.
;
illegal_handler:
    INC   R2, #1                  ; count handler invocations
    RDSPR R9, EPC
    INC   R9, #4
    RDSPR R10, ESR
    IRET  R10, R9

; ── Main test ────────────────────────────────────────────
.org 0x80
start:
    LLI R2, #0                  ; R2 = handler call count

    ; Test 1: undefined Format L op=7 (reserved)
    ; Encoding: [01][111][Rd=0000][spare=0000000][imm16=0x0000] = 0x7C000000
    .word 0x7C000000

    ; Handler should have incremented R2 to 1
    CMPI R2, #1
    BNE fail

    ; Test 2: MUL R3, R4 — defined in ISA but no microcode (sentinel)
    MUL R3, R4

    ; Handler should have incremented R2 to 2
    CMPI R2, #2
    BNE fail

    ; Pass
    LLI R1, #1
fail:
    BREAK
