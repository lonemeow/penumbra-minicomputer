; test_fib.s — Fibonacci test (tail-recursive / iterative)
;
; Calling convention:
;   Result returned in R1
;   Returns via RET (JMP R13)
;
; Preamble simulates boot ROM: sets LR, calls test, halts on return.

; ── Boot preamble ────────────────────────────────────────────
        LLI  LR, #halt         ; R13 = return address
        B    fib               ; call test

halt:
        B    halt              ; infinite loop — testbench detects this

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
