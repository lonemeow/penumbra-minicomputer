/*	$NetBSD$	*/

/*
 * User/kernel copy functions.
 *
 * Each function sets pcb_onfault before accessing user memory,
 * then clears it on success.  If a page fault occurs during the
 * access, the trap handler redirects execution to _copy_fault
 * via the trapframe (sets tf_epc = pcb_onfault), which returns
 * EFAULT to the original caller through the normal RTE path.
 *
 * This is the standard NetBSD onfault pattern (cf. MIPS copy.S).
 */

#include "assym.h"
#include <sys/errno.h>

/*
 * Helper macro: load current PCB pointer into a register.
 *   curlwp = cpu_info_store.ci_curlwp
 *   pcb    = curlwp->l_addr (= l_pcb)
 */
#define GET_PCB(reg)				\
	la	reg, cpu_info_store;		\
	ldw	reg, [reg + CI_CURLWP];		\
	ldw	reg, [reg + L_PCB]

/*
 * VM_MAXUSER_ADDRESS — user addresses must be below this.
 * 2G/2G split: user = 0x0000_0000 .. 0x7FFF_FFFF.
 */
#define VM_MAXUSER_ADDR	0x80000000

	.text

/* ── copyin(uaddr, kaddr, len) ─────────────────────────────────
 *
 * Copy len bytes from user address uaddr to kernel address kaddr.
 * Returns 0 on success, EFAULT on bad user address.
 *
 * Calling convention: R1=uaddr, R2=kaddr, R3=len
 * memcpy convention:  R1=dst,   R2=src,   R3=len
 */
	.globl	copyin
	.type	copyin, @function
copyin:
	/* Save callee-saved regs + LR (we call memcpy) */
	sub	r14, #12
	stw	r13, [r14 + 8]		/* LR */
	stw	r5,  [r14 + 4]		/* callee-saved (holds PCB) */
	stw	r6,  [r14 + 0]		/* callee-saved (scratch) */

	/* Validate user address range: uaddr + len <= VM_MAXUSER_ADDR */
	mov	r11, r1			/* r11 = uaddr */
	add	r11, r3			/* r11 = uaddr + len */
	bcs	_copy_efault_stack	/* unsigned overflow → fault */
	li	r6, VM_MAXUSER_ADDR
	cmp	r11, r6
	bhi	_copy_efault_stack	/* above user space → fault */

	/* Set pcb_onfault = _copy_fault */
	GET_PCB(r5)
	la	r11, _copy_fault
	stw	r11, [r5 + PCB_ONFAULT]

	/* Rearrange args for memcpy(dst=kaddr, src=uaddr, len) */
	mov	r11, r1			/* r11 = uaddr (save) */
	mov	r1, r2			/* dst = kaddr */
	mov	r2, r11			/* src = uaddr */
	/* r3 = len (already in place) */
	bl	memcpy

	/* Success: clear onfault, return 0 */
	stw	r0, [r5 + PCB_ONFAULT]	/* r0 = zero reg */
	lli	r1, #0
	ldw	r6,  [r14 + 0]
	ldw	r5,  [r14 + 4]
	ldw	r13, [r14 + 8]
	add	r14, #12
	ret

/* ── copyout(kaddr, uaddr, len) ────────────────────────────────
 *
 * Copy len bytes from kernel address kaddr to user address uaddr.
 * Returns 0 on success, EFAULT on bad user address.
 *
 * Calling convention: R1=kaddr, R2=uaddr, R3=len
 * memcpy convention:  R1=dst,   R2=src,   R3=len
 */
	.globl	copyout
	.type	copyout, @function
copyout:
	sub	r14, #12
	stw	r13, [r14 + 8]
	stw	r5,  [r14 + 4]
	stw	r6,  [r14 + 0]

	/* Validate: uaddr + len <= VM_MAXUSER_ADDR */
	mov	r11, r2			/* r11 = uaddr */
	add	r11, r3			/* r11 = uaddr + len */
	bcs	_copy_efault_stack	/* overflow → fault */
	li	r6, VM_MAXUSER_ADDR
	cmp	r11, r6
	bhi	_copy_efault_stack	/* above user space → fault */

	/* Set pcb_onfault */
	GET_PCB(r5)
	la	r11, _copy_fault
	stw	r11, [r5 + PCB_ONFAULT]

	/* memcpy(dst=uaddr, src=kaddr, len) — args already correct:
	 *   R1=kaddr → src, R2=uaddr → dst — need to swap */
	mov	r11, r1			/* r11 = kaddr */
	mov	r1, r2			/* dst = uaddr */
	mov	r2, r11			/* src = kaddr */
	/* r3 = len (already in place) */
	bl	memcpy

	/* Success */
	stw	r0, [r5 + PCB_ONFAULT]
	lli	r1, #0
	ldw	r6,  [r14 + 0]
	ldw	r5,  [r14 + 4]
	ldw	r13, [r14 + 8]
	add	r14, #12
	ret

/* ── copyinstr(uaddr, kaddr, maxlen, *done) ────────────────────
 *
 * Copy NUL-terminated string from user to kernel.
 * Copies at most maxlen bytes (including NUL).
 * If done != NULL, stores actual bytes copied.
 * Returns 0 on success, EFAULT on fault, ENAMETOOLONG if no NUL.
 *
 * R1=uaddr, R2=kaddr, R3=maxlen, R4=done
 */
	.globl	copyinstr
	.type	copyinstr, @function
copyinstr:
	sub	r14, #20
	stw	r13, [r14 + 16]
	stw	r5,  [r14 + 12]		/* PCB */
	stw	r6,  [r14 + 8]		/* kaddr (dst) */
	stw	r7,  [r14 + 4]		/* maxlen counter */
	stw	r8,  [r14 + 0]		/* done ptr */

	mov	r8, r4			/* save done pointer */
	mov	r7, r3			/* save maxlen */
	mov	r6, r2			/* save kaddr */

	/* Validate: uaddr must be below VM_MAXUSER_ADDR */
	li	r11, VM_MAXUSER_ADDR
	cmp	r1, r11
	bhs	_copystr_efault

	/* Set pcb_onfault */
	GET_PCB(r5)
	la	r11, _copy_fault_str
	stw	r11, [r5 + PCB_ONFAULT]

	/* Byte-by-byte copy loop */
	mov	r2, r1			/* r2 = uaddr (src cursor) */
	mov	r3, r6			/* r3 = kaddr (dst cursor) */
	lli	r4, #0			/* r4 = count */

_copyinstr_loop:
	cmp	r4, r7
	beq	_copyinstr_toolong	/* maxlen reached, no NUL found */
	ldb	r11, [r2]		/* load byte from user */
	stb	r11, [r3]		/* store byte to kernel */
	add	r2, #1
	add	r3, #1
	add	r4, #1
	cmp	r11, r0			/* NUL? */
	bne	_copyinstr_loop

	/* NUL found — success */
	stw	r0, [r5 + PCB_ONFAULT]
	cmp	r8, r0
	beq	_copyinstr_done
	stw	r4, [r8]		/* *done = count */
_copyinstr_done:
	lli	r1, #0
	ldw	r8,  [r14 + 0]
	ldw	r7,  [r14 + 4]
	ldw	r6,  [r14 + 8]
	ldw	r5,  [r14 + 12]
	ldw	r13, [r14 + 16]
	add	r14, #20
	ret

_copyinstr_toolong:
	stw	r0, [r5 + PCB_ONFAULT]
	cmp	r8, r0
	beq	1f
	stw	r4, [r8]		/* *done = maxlen */
1:
	lli	r1, #ENAMETOOLONG
	ldw	r8,  [r14 + 0]
	ldw	r7,  [r14 + 4]
	ldw	r6,  [r14 + 8]
	ldw	r5,  [r14 + 12]
	ldw	r13, [r14 + 16]
	add	r14, #20
	ret

/* ── copyoutstr(kaddr, uaddr, maxlen, *done) ───────────────────
 *
 * Copy NUL-terminated string from kernel to user.
 * Same semantics as copyinstr but reversed direction.
 *
 * R1=kaddr, R2=uaddr, R3=maxlen, R4=done
 */
	.globl	copyoutstr
	.type	copyoutstr, @function
copyoutstr:
	sub	r14, #20
	stw	r13, [r14 + 16]
	stw	r5,  [r14 + 12]
	stw	r6,  [r14 + 8]
	stw	r7,  [r14 + 4]
	stw	r8,  [r14 + 0]

	mov	r8, r4
	mov	r7, r3

	/* Validate: uaddr must be below VM_MAXUSER_ADDR */
	li	r11, VM_MAXUSER_ADDR
	cmp	r2, r11
	bhs	_copystr_efault

	/* Set pcb_onfault */
	GET_PCB(r5)
	la	r11, _copy_fault_str
	stw	r11, [r5 + PCB_ONFAULT]

	/* Byte-by-byte: R1=kaddr (src), R2=uaddr (dst) */
	mov	r3, r2			/* r3 = uaddr (dst cursor) */
	mov	r2, r1			/* r2 = kaddr (src cursor) */
	lli	r4, #0			/* count */

_copyoutstr_loop:
	cmp	r4, r7
	beq	_copyoutstr_toolong
	ldb	r11, [r2]
	stb	r11, [r3]
	add	r2, #1
	add	r3, #1
	add	r4, #1
	cmp	r11, r0
	bne	_copyoutstr_loop

	/* Success */
	stw	r0, [r5 + PCB_ONFAULT]
	cmp	r8, r0
	beq	_copyoutstr_done
	stw	r4, [r8]
_copyoutstr_done:
	lli	r1, #0
	ldw	r8,  [r14 + 0]
	ldw	r7,  [r14 + 4]
	ldw	r6,  [r14 + 8]
	ldw	r5,  [r14 + 12]
	ldw	r13, [r14 + 16]
	add	r14, #20
	ret

_copyoutstr_toolong:
	stw	r0, [r5 + PCB_ONFAULT]
	cmp	r8, r0
	beq	1f
	stw	r4, [r8]
1:
	lli	r1, #ENAMETOOLONG
	ldw	r8,  [r14 + 0]
	ldw	r7,  [r14 + 4]
	ldw	r6,  [r14 + 8]
	ldw	r5,  [r14 + 12]
	ldw	r13, [r14 + 16]
	add	r14, #20
	ret

/* ── ufetch / ustore ───────────────────────────────────────────
 *
 * Single-value user memory access with fault recovery.
 * R1 = user address, R2 = value pointer (fetch) or value (store).
 */

	.globl	_ufetch_8
	.type	_ufetch_8, @function
_ufetch_8:
	li	r11, VM_MAXUSER_ADDR
	cmp	r1, r11
	bhs	_ufetchstore_efault
	GET_PCB(r3)
	la	r11, _ufetchstore_fault
	stw	r11, [r3 + PCB_ONFAULT]
	ldb	r11, [r1]
	stb	r11, [r2]
	stw	r0, [r3 + PCB_ONFAULT]
	lli	r1, #0
	ret

	.globl	_ufetch_16
	.type	_ufetch_16, @function
_ufetch_16:
	li	r11, VM_MAXUSER_ADDR
	cmp	r1, r11
	bhs	_ufetchstore_efault
	GET_PCB(r3)
	la	r11, _ufetchstore_fault
	stw	r11, [r3 + PCB_ONFAULT]
	ldh	r11, [r1]
	sth	r11, [r2]
	stw	r0, [r3 + PCB_ONFAULT]
	lli	r1, #0
	ret

	.globl	_ufetch_32
	.type	_ufetch_32, @function
_ufetch_32:
	li	r11, VM_MAXUSER_ADDR
	cmp	r1, r11
	bhs	_ufetchstore_efault
	GET_PCB(r3)
	la	r11, _ufetchstore_fault
	stw	r11, [r3 + PCB_ONFAULT]
	ldw	r11, [r1]
	stw	r11, [r2]
	stw	r0, [r3 + PCB_ONFAULT]
	lli	r1, #0
	ret

	.globl	_ustore_8
	.type	_ustore_8, @function
_ustore_8:
	li	r11, VM_MAXUSER_ADDR
	cmp	r1, r11
	bhs	_ufetchstore_efault
	GET_PCB(r3)
	la	r11, _ufetchstore_fault
	stw	r11, [r3 + PCB_ONFAULT]
	stb	r2, [r1]
	stw	r0, [r3 + PCB_ONFAULT]
	lli	r1, #0
	ret

	.globl	_ustore_16
	.type	_ustore_16, @function
_ustore_16:
	li	r11, VM_MAXUSER_ADDR
	cmp	r1, r11
	bhs	_ufetchstore_efault
	GET_PCB(r3)
	la	r11, _ufetchstore_fault
	stw	r11, [r3 + PCB_ONFAULT]
	sth	r2, [r1]
	stw	r0, [r3 + PCB_ONFAULT]
	lli	r1, #0
	ret

	.globl	_ustore_32
	.type	_ustore_32, @function
_ustore_32:
	li	r11, VM_MAXUSER_ADDR
	cmp	r1, r11
	bhs	_ufetchstore_efault
	GET_PCB(r3)
	la	r11, _ufetchstore_fault
	stw	r11, [r3 + PCB_ONFAULT]
	stw	r2, [r1]
	stw	r0, [r3 + PCB_ONFAULT]
	lli	r1, #0
	ret

/* ── Fault recovery stubs ──────────────────────────────────────
 *
 * _copy_fault: reached when trap handler redirects tf_epc here
 * after a page fault during copyin/copyout.  The trap handler
 * has already cleared pcb_onfault.
 * Stack frame: 12 bytes (LR, R5, R6) from copyin/copyout.
 */
_copy_efault_stack:
	/* Address validation failed — no onfault was set yet */
	lli	r1, #EFAULT
	ldw	r6,  [r14 + 0]
	ldw	r5,  [r14 + 4]
	ldw	r13, [r14 + 8]
	add	r14, #12
	ret

_copy_fault:
	/* Arrived here via RTE — trap handler set tf_epc to this
	 * address and cleared pcb_onfault.  Restore stack frame
	 * and return EFAULT to caller. */
	lli	r1, #EFAULT
	ldw	r6,  [r14 + 0]
	ldw	r5,  [r14 + 4]
	ldw	r13, [r14 + 8]
	add	r14, #12
	ret

/* Fault during copyinstr/copyoutstr — 20-byte stack frame */
_copystr_efault:
_copy_fault_str:
	lli	r1, #EFAULT
	ldw	r8,  [r14 + 0]
	ldw	r7,  [r14 + 4]
	ldw	r6,  [r14 + 8]
	ldw	r5,  [r14 + 12]
	ldw	r13, [r14 + 16]
	add	r14, #20
	ret

/* Fault during ufetch/ustore — leaf functions, no stack frame */
_ufetchstore_efault:
_ufetchstore_fault:
	lli	r1, #EFAULT
	ret
