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
    STW  r3, [r1]
    add  r1, 4
    b    bss_loop
    bss_done:

    // Call constructors
    la   r1, _init_array_start
    la   r2, _init_array_end
    init_loop:
    cmp  r1, r2
    bhs  init_done
    ldw  r3, [r1]
    // Call function pointer in r3
    // We need to save r1/r2 if the constructor doesn't follow ABI
    // But constructors are standard functions, they should save callee-saved.
    // However, we are using scratch registers here. Let's use R5/R6 for loop
    mov  r5, r1
    mov  r6, r2
    jalr r3
    mov  r1, r5
    mov  r2, r6
    add  r1, 4
    b    init_loop
    init_done:

    // Call main
    bl   main

    // Save return value
    mov  r7, r1

    // Call destructors
    la   r1, _fini_array_start
    la   r2, _fini_array_end
fini_loop:
    cmp  r1, r2
    bhs  fini_done
    ldw  r3, [r1]
    mov  r5, r1
    mov  r6, r2
    jalr r3
    mov  r1, r5
    mov  r2, r6
    add  r1, 4
    b    fini_loop
fini_done:

    // Pass return value (r7) to exit
    // syscall exit(status) -> r11=1, r1=status
    mov  r1, r7
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
