/*	$NetBSD$	*/

/*
 * Penumbra atomic compare-and-swap using RAS (Restartable Atomic Sequences).
 *
 * The critical load-compare-store sequence contains no privileged
 * instructions.  If a timer interrupt (or any exception) preempts
 * execution inside the RAS region, the kernel's userret() detects
 * this via ras_lookup() and resets PC to the start of the sequence,
 * retrying it from the beginning.
 *
 * The RAS region must be registered by userland via rasctl() before
 * first use — see atomic_ras_init.c for the constructor that does this.
 *
 * uint32_t _atomic_cas_32(volatile uint32_t *ptr, uint32_t old, uint32_t new)
 *   R1 = ptr, R2 = old, R3 = new
 *   Returns previous value in R1.
 */

	.text

	.globl	_atomic_cas_32
	.type	_atomic_cas_32, @function

	/* RAS boundary labels — used by atomic_ras_init.c for registration */
	.globl	_atomic_cas_32_ras_start
	.globl	_atomic_cas_32_ras_end

_atomic_cas_32:
_atomic_cas_32_ras_start:
	ldw	r11, [r1]	/* r11 = *ptr */
	cmp	r11, r2		/* compare with expected */
	bne	1f
	stw	r3, [r1]	/* *ptr = new (if matched) */
_atomic_cas_32_ras_end:
1:
	mov	r1, r11		/* return previous value */
	ret

	.globl	atomic_cas_32
	.set	atomic_cas_32, _atomic_cas_32
	.globl	atomic_cas_32_ni
	.set	atomic_cas_32_ni, _atomic_cas_32

	/* ILP32: uint = ulong = ptr = 32-bit */
	.globl	_atomic_cas_uint
	.set	_atomic_cas_uint, _atomic_cas_32
	.globl	atomic_cas_uint
	.set	atomic_cas_uint, _atomic_cas_32
	.globl	_atomic_cas_uint_ni
	.set	_atomic_cas_uint_ni, _atomic_cas_32
	.globl	atomic_cas_uint_ni
	.set	atomic_cas_uint_ni, _atomic_cas_32

	.globl	_atomic_cas_ulong
	.set	_atomic_cas_ulong, _atomic_cas_32
	.globl	atomic_cas_ulong
	.set	atomic_cas_ulong, _atomic_cas_32
	.globl	_atomic_cas_ulong_ni
	.set	_atomic_cas_ulong_ni, _atomic_cas_32
	.globl	atomic_cas_ulong_ni
	.set	atomic_cas_ulong_ni, _atomic_cas_32

	.globl	_atomic_cas_ptr
	.set	_atomic_cas_ptr, _atomic_cas_32
	.globl	atomic_cas_ptr
	.set	atomic_cas_ptr, _atomic_cas_32
	.globl	_atomic_cas_ptr_ni
	.set	_atomic_cas_ptr_ni, _atomic_cas_32
	.globl	atomic_cas_ptr_ni
	.set	atomic_cas_ptr_ni, _atomic_cas_32
