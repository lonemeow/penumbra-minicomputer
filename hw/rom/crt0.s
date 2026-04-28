// crt0.s — Penumbra boot ROM startup
//
// CPU enters here at RESET_PC (0xFFFF_0000).  Pre-loads UART base +
// THRE mask in scratch registers (so the early-print helpers in
// ram_check.s can use them without re-deriving), runs the pre-stack
// RAM diagnostic, sets up the C stack, and calls main().
//
// If main() ever returns, BREAK halts the CPU.  The RAM diagnostic
// itself, along with _early_puts / _dump_r5 / _early_putc, lives in
// ram_check.s.
//
// Assembled with:  llvm-mc -triple=penumbra -filetype=obj crt0.s -o crt0.o
// This must be linked first so _start lands at RESET_PC.

        .section .text._start,"ax",@progbits
        .globl  _start
        .type   _start,@function

.equ UART_BASE, 0xFF000000
.equ LSR_THRE,  0x20

_start:
        // Pre-load UART base + THRE mask once; reused by _early_putc.
        li    r12, UART_BASE
        lli   r11, LSR_THRE

        bl    _ram_check

        // Set up stack pointer (top of page 2, grows down)
        li    r14, 0x2000
        // Call main
        bl    main
        break

        .size   _start, . - _start
