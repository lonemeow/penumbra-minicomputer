/*	$NetBSD$	*/

/*
 * Penumbra atomic compare-and-swap using interrupt disable.
 *
 * This is the uniprocessor implementation: RDSPR SR / DI / ... / WRSPR SR.
 * TODO: Replace with RAS (Restartable Atomic Sequences) once kernel
 * RAS infrastructure is in place.
 *
 * uint32_t _atomic_cas_32(volatile uint32_t *ptr, uint32_t old, uint32_t new)
 *   R1 = ptr, R2 = old, R3 = new
 *   Returns previous value in R1.
 */

	.text

	.globl	_atomic_cas_32
	.type	_atomic_cas_32, @function
_atomic_cas_32:
	rdspr	r4, sr		/* save SR */
	di			/* disable interrupts */
	ldw	r11, [r1]	/* r11 = *ptr */
	cmp	r11, r2		/* compare with expected */
	bne	1f
	stw	r3, [r1]	/* *ptr = new (if matched) */
1:
	wrspr	sr, r4		/* restore SR (re-enable IRQs if they were on) */
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
