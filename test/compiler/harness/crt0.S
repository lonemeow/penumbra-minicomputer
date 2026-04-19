// test/compiler/harness/crt0.S
// Minimal startup for compiler correctness tests.

.section .text._start
.global _start
_start:
    // Initialize stack pointer to top of RAM (16 MB)
    lli  r14, #0
    lui  r14, #0x0100

    // Clear BSS
    la   r1, _bss_start
    la   r2, _bss_end
    lli  r3, #0
bss_loop:
    cmp  r1, r2
    bhs  bss_done
    stw  r3, [r1]
    add  r1, #4
    b    bss_loop
bss_done:

    // Call main
    bl   main

    // Pass return value (r1) to exit
    // syscall exit(status) -> r11=1, r1=status
    // r1 already contains return value from main
    lli  r11, #1
    syscall

    // Should not reach here
    break

.global abort
abort:
    lli  r1, #127 // SIGABRT-ish
    lli  r11, #1
    syscall
    break
