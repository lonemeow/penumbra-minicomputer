; test_fib.s — Fibonacci test (tail-recursive / iterative)
;
; Convention: R1 = 1 on pass, R1 = 0 on fail.
; Computes fib(10), checks result = 55.

; ── Boot preamble ────────────────────────────────────────────
        LLI  LR, #check        ; R13 = return address
        B    fib               ; call test

check:
        ; R1 = fib(10) result, expect 55
        CMPI R1, #55
        BNE  fail
        LLI  R1, #1            ; PASS
        B    halt
fail:
        LLI  R1, #0            ; FAIL
halt:
        B    halt              ; testbench detects stable PC

; ── fib: compute fib(N) iteratively ──────────────────────────
; Algorithm: a=0, b=1, iterate N times: (a, b) = (b, a+b)
; Result: R1 = fib(N)
;
; Register usage:
;   R1 = N (input), then result (output)
;   R3 = a (current fibonacci number)
;   R4 = b (next fibonacci number)
;   R5 = loop counter i
;   R6 = temp for swap

fib:
        LLI  R1, #10          ; N = 10
        LLI  R3, #0           ; a = fib(0) = 0
        LLI  R4, #1           ; b = fib(1) = 1
        LLI  R5, #0           ; i = 0
        CMP  R1, R0
        B    start_loop

loop:
        MOV  R9, R4
        ADD  R4, R3
        MOV  R3, R9
        DEC  R1, #1
start_loop:
        BNZ  loop

fib_done:
        MOV  R1, R3           ; result = a = fib(N)
        RET                   ; return to caller
