; trap_entry.s — Per-trap entry trampolines
;
; Each trap vector points to a small trampoline that loads the trap
; number into R1 (first argument), switches to a dedicated trap stack,
; calls the C handler, then halts with BREAK.
;
; The trap stack is at a fixed high RAM address (0x00FFFC00), separate
; from the boot stack, so C code in the handler can use the stack
; without corrupting the interrupted function's frame.
;
; void unhandled_trap(int trapno, unsigned int epc, unsigned int fault_addr);

        .text

; Macro: set up trap stack + call handler with trap number, EPC, and fault addr.
; void unhandled_trap(int trapno, unsigned int epc, unsigned int fault_addr);
; Clobbers R1-R3 and R14 — fine since we BREAK after.
.macro TRAP_ENTRY num
        rdspr   r2, epc                 ; R2 = EPC (arg 2)
        rdsys   r3, #0, #1              ; R3 = MMU FAULT_ADDR (arg 3)
        lli     r1, \num                ; R1 = trap number (arg 1)
        li      r14, 0x00FFFC00         ; trap stack (1 KB below main stack top)
        bl      unhandled_trap
        break
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
