; crt0.s — Penumbra boot ROM startup stub
;
; Minimal C runtime entry point.  Sets up the stack pointer and calls
; main().  If main() returns, halts the CPU with BREAK.
;
; Assembled with:  llvm-mc -triple=penumbra -filetype=obj crt0.s -o crt0.o
;
; This must be linked first so _start lands at RESET_PC (0xFFFF_E000).

        .section .text._start,"ax",@progbits
        .globl  _start
        .type   _start,@function

_start:
        ; Set up stack pointer (top of page 2, grows down)
        li      r14, 0x2000

        ; Call main — returns in R1
        bl      main

        ; main() returned: halt
        break

        .size   _start, . - _start
