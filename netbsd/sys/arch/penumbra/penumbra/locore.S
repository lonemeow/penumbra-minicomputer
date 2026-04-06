/*	$NetBSD$	*/

/*
 * Penumbra low-level kernel entry and exception handling.
 *
 * Entry point: _start (jumped to by bootloader with MMU OFF,
 * R1 = bootinfo physical address, running in supervisor mode).
 *
 * The bootloader loads the kernel ELF at an arbitrary physical
 * address and jumps with:
 *   R1  = bootinfo physical address
 *   R2  = kernel physical entry point
 *   SR.S = 1 (supervisor), SR.M = 0 (MMU off), SR.I = 0 (irqs off)
 *
 * The kernel is linked at KERNEL_TEXT_BASE (0x8001_0000).
 * locore.S must: compute virt-to-phys bias, set up initial TLB
 * entries, enable the MMU, and jump to virtual addresses before
 * any C code can run.
 *
 * Exception vectors live at physical 0x0000_0000:
 *   0x00: reset        0x04: IRQ
 *   0x08: TLB miss     0x0C: TLB protection
 *   0x10: bus fault    0x14: BREAK
 *   0x18: SYSCALL      0x1C: illegal / privilege
 */

#include "assym.h"
#include <machine/sysreg.h>
#include <machine/pmap.h>

	.text
	.globl	_start
	.type	_start, @function

/*
 * Kernel entry point.
 * Called by bootloader with:
 *   R1 = bootinfo physical address
 *   R2 = kernel physical entry point
 *   SR.S = 1 (supervisor), SR.M = 0 (MMU off), SR.I = 0 (irqs off)
 *
 * Register allocation for early boot:
 *   R5 = saved bootinfo physical address
 *   R6 = phys_bias (physical - virtual offset)
 */
_start:
	/* Save bootinfo pointer — R1 will be clobbered */
	mov	r5, r1

	/*
	 * Compute phys_bias = physical_addr - virtual_addr.
	 * LA loads the link-time (virtual) address of _start_bias;
	 * mov r6, pc at that label reads the runtime (physical) address.
	 */
	la	r1, _start_bias
_start_bias:
	mov	r6, pc
	sub	r6, r1

	/*
	 * Set up early boot stack (physical address).
	 * The boot stack lives in BSS — physical memory is already
	 * allocated by the ELF loader (p_memsz).  Convert the
	 * link-time virtual address to physical using the bias.
	 * Stack grows downward, so point SP at the top.
	 */
	la	sp, _boot_stack_top
	add	sp, r6

	/* Flush the entire TLB — invalidate all slots */
	mov	r1, r0
.Lflush_tlb:
	wrsys	r1, #SYSDEV_MMU, #MMU_TLB_INDEX
	wrsys	r0, #SYSDEV_MMU, #MMU_TLB_PTE
	add	r1, #1
	cmp	r1, #TLB_NSLOTS
	blo	.Lflush_tlb

	/* Invalidate I-cache — stale bootloader code may be cached */
	wrsys	r0, #SYSDEV_ICACHE, #CACHE_INVAL

	/*
	 * Next: set up pinned TLB entries (handler page, PGD),
	 * install TLB miss handler, build page directory,
	 * enable MMU, jump to virtual addresses.
	 */
	break

/*
 * cpu_switchto(oldlwp, newlwp, returning)
 * Switch context from oldlwp to newlwp.
 * Stub — will be implemented for scheduler support.
 */
	.globl	cpu_switchto
	.type	cpu_switchto, @function
cpu_switchto:
// TODO: Not implemented
	break

/*
 * setjmp / longjmp for kernel use (label_t).
 * Save/restore callee-saved registers.
 */
	.globl	setjmp
	.type	setjmp, @function
setjmp:
	/* R1 = pointer to label_t
	 * Save callee-saved regs + LR + SP.
	 * No need to save PC — LR already holds our caller's return address.
	 */
	stw	r5,  [r1 + 0]
	stw	r6,  [r1 + 4]
	stw	r7,  [r1 + 8]
	stw	r8,  [r1 + 12]
	stw	r9,  [r1 + 16]
	stw	r10, [r1 + 20]
	stw	r12, [r1 + 24]
	stw	r13, [r1 + 28]
	stw	r14, [r1 + 32]
	mov	r1, r0		/* return 0 */
	ret

	.globl	longjmp
	.type	longjmp, @function
longjmp:
	/* R1 = pointer to label_t
	 * Restore callee-saved regs + LR + SP, return 1.
	 * After restoring R13 (LR), ret jumps back to setjmp's caller.
	 */
	ldw	r5,  [r1 + 0]
	ldw	r6,  [r1 + 4]
	ldw	r7,  [r1 + 8]
	ldw	r8,  [r1 + 12]
	ldw	r9,  [r1 + 16]
	ldw	r10, [r1 + 20]
	ldw	r12, [r1 + 24]
	ldw	r13, [r1 + 28]
	ldw	r14, [r1 + 32]
	lli	r1, 1		/* return 1 */
	ret

/*
 * Early boot stack — used before MMU is enabled.
 * 4 KB is enough for the pre-MMU TLB setup code.
 */
	.bss
	.align	4
	.globl	_boot_stack
_boot_stack:
	.space	4096
	.globl	_boot_stack_top
_boot_stack_top:
