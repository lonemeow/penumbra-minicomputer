// test/compiler/harness/crt0.S
// Minimal startup for compiler correctness tests.

.section .text._start
.global _start
_start:
    // Initialize stack pointer to top of RAM (16 MB)
    LUI  R14, #0x0100
    // LLI  R14, #0x0000 // Already zeroed by LUI if not careful, but Penumbra LUI is Rd | (imm << 16)
    // Actually our LUI is OR-based, so better to LLI first then LUI.
    LLI  R14, #0x0000
    LUI  R14, #0x0100

    // Clear BSS
    LA   R1, _bss_start
    LA   R2, _bss_end
    LLI  R3, #0
bss_loop:
    CMP  R1, R2
    BHS  bss_done
    STW  R3, [R1]
    ADD  R1, #4
    B    bss_loop
bss_done:

    // Call main
    BL   main

    // Pass return value (R1) to exit
    // syscall exit(status) -> R11=1, R1=status
    // R1 already contains return value from main
    LLI  R11, #1
    SYSCALL

    // Should not reach here
    BREAK

.global abort
abort:
    LLI  R1, #127 // SIGABRT-ish
    LLI  R11, #1
    SYSCALL
    BREAK
