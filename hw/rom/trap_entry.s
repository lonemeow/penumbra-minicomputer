// trap_entry.s — Per-trap entry trampolines
//
// Each trap vector points to a small trampoline that loads the trap
// number into R1 (first argument), switches to a dedicated trap stack,
// calls the C handler, then halts with BREAK.
//
// The trap stack is at a fixed high RAM address (0x00FFFC00), separate
// from the boot stack, so C code in the handler can use the stack
// without corrupting the interrupted function's frame.
//
// SPR/sysreg reads (EPC, ESR, FAULT_ADDR, etc.) are done in C via
// inline asm — see penumbra.h.  This keeps the trampolines minimal
// and makes it easy to add new diagnostic output.
//
// void unhandled_trap(int trapno);

        .text

// Macro: set up trap stack + call handler with trap number.
.macro TRAP_ENTRY num
        lli     r1, \num                // R1 = trap number (arg 1)
        li      r14, 0x00FFFC00         // trap stack (1 KB below main stack top)
        bl      unhandled_trap
        b       _halt
.endm

        .globl  _trap_bus_fault
_trap_bus_fault:
        TRAP_ENTRY 0

        .globl  _trap_irq
_trap_irq:
        TRAP_ENTRY 1

        .globl  _trap_tlb_miss
_trap_tlb_miss:
        TRAP_ENTRY 2

        .globl  _trap_tlb_prot
_trap_tlb_prot:
        TRAP_ENTRY 3

        .globl  _trap_priv
_trap_priv:
        TRAP_ENTRY 4

        .globl  _trap_syscall
_trap_syscall:
        TRAP_ENTRY 5

        .globl  _trap_break
_trap_break:
        TRAP_ENTRY 6

        .globl  _trap_illegal
_trap_illegal:
        TRAP_ENTRY 7

        .globl  _trap_align
_trap_align:
        TRAP_ENTRY 8

_halt:
        b _halt

// ── Bus fault ignore handler ────────────────────────────────────────
// Advances EPC past the faulting instruction and resumes via ERET.
// Used during RAM detection to silently skip faulting probes.
// Uses R12 as scratch (saved/restored via PC-relative storage).
        .globl  _trap_bus_ignore
_trap_bus_ignore:
        stw     r12, [pc + .Ltrap_scratch - .]
        rdspr   r12, epc
        add     r12, 4
        wrspr   epc, r12
        ldw     r12, [pc + .Ltrap_scratch - .]
        eret
.Ltrap_scratch:
        .long   0
