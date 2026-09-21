/*	$NetBSD$	*/

/*
 * Penumbra atomic compare-and-swap, as a restartable sequence.
 *
 * The load-compare-store below is not atomic on its own.  A trap taken
 * inside it rewinds PC to the start instead of resuming, so the sequence
 * redoes rather than committing a value computed before the handler ran.
 * Userland is restarted from userret() against the range rasctl(2)
 * registers (atomic_ras_init.c); supervisor mode is restarted by the
 * kernel's trap path, which reads the same two markers.
 *
 * Restart is sound because of two properties of the region, both
 * enforced below: the store is its last instruction, and nothing inside
 * writes an argument register, so re-entering at the start is
 * indistinguishable from never having begun.
 *
 * The end marker sits after the store, not on it — a trap with PC at
 * the store restarts, because the store has not executed.  PC equal to
 * the marker is the one excluded case, and means the sequence is over:
 * the store retired, or the compare failed and branched here.  Both
 * arrive at the same address, so one test covers them.
 *
 * uint32_t _atomic_cas_32(volatile uint32_t *ptr, uint32_t old, uint32_t new)
 *   R1 = ptr, R2 = old, R3 = new
 *   Returns previous value in R1.
 */

	.text

	.globl	_atomic_cas_32
	.type	_atomic_cas_32, @function

	/* Region markers — read by both restart paths */
	.globl	_atomic_cas_32_ras_start
	.globl	_atomic_cas_32_ras_end

_atomic_cas_32:
_atomic_cas_32_ras_start:
	ldw	r11, [r1]	/* r11 = *ptr */
	cmp	r11, r2		/* compare with expected */
	bne	1f
	stw	r3, [r1]	/* *ptr = new (if matched) */
_atomic_cas_32_ras_end:		/* one past the store, not on it: an
				   interrupted store restarts, arriving
				   here means the sequence is done */
1:
	mov	r1, r11		/* return previous value */
	ret

	/*
	 * Growing the region changes what a restart redoes, and both
	 * restart paths depend on the store staying last — so that is a
	 * decision to be made deliberately, not an edit to be made in
	 * passing.
	 */
	.if (_atomic_cas_32_ras_end - _atomic_cas_32_ras_start) != 16
	.error "atomic_cas_32 restartable region changed size"
	.endif

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
