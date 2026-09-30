; bp_recurse.s -- recursion six deep from one call site.
;
; Run with +bpmodel; the window runs from reset to the BREAK.  _start
; calls rec, which calls itself from one site until R4 counts down to
; zero, keeping each return address on a stack in RAM.  Five returns go
; back to that site and the last to _start.  A 4-deep ring holds the
; site's address in every slot, so a plain ring predicts the fifth return
; from a stale slot and gets it right, while one that counts its entries
; finds itself empty; both miss the last.  run_iss_tests.py derives the
; counts.

_start:
    LLI  R14, #0x1000          ; a stack in RAM
    LLI  R4, #6
    BL   rec
    BREAK

rec:
    SUB  R4, #1
    BEQ  rec_base              ; forward: taken once, at the deepest call
    SUB  R14, #4
    STW  R13, [R14]
    BL   rec                   ; the one recursive call site
    LDW  R13, [R14]
    ADD  R14, #4
rec_base:
    JMP  R13
