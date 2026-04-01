; test_bus_ignore.s — Minimal bus fault + ERET test
;
; Install ERET-only handler, trigger one bus fault on a store,
; then BREAK immediately. If the CPU hangs after ERET, the
; testbench times out (FAIL). If it resumes, we hit BREAK (PASS).

_start:
    LLI  R1, #0               ; assume fail

    ; Install handler
    LA   R2, #bus_ignore
    STW  R2, [R0 + #0]        ; vector[0] = bus_ignore

    ; Trigger bus fault on store to unmapped address
    LI   R5, #0x02000000
    STW  R0, [R5]             ; FAULT — handler ERETs, resumes here:

    ; If we get here, the ERET worked
    LLI  R1, #1               ; PASS
    BREAK

bus_ignore:
    RDSPR R2, EPC
    ADD   R2, #4
    WRSPR EPC, R2
    ERET
