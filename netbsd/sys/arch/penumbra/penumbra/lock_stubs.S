/*	$NetBSD$	*/

/*
 * Penumbra lock stubs: the uncontended half of mutex_enter() and
 * mutex_exit(), as restartable sequences.
 *
 * Without these, kern_mutex.c aliases both entry points straight to the
 * C vector routines, so an uncontended acquire pays a register-saving
 * frame and several nested calls that exist only to serve contention,
 * turnstiles and aborts.  Each stub is a leaf that handles the one case
 * worth handling inline and branches to the vector routine for anything
 * else, which keeps the slow path exactly where it was.
 *
 * Atomicity comes from restart, not exclusion: the load-compare-store
 * below is bare, and a trap taken inside it rewinds PC to the start
 * rather than resuming, so the store can never commit a value read
 * before the handler ran.  This is the same contract atomic_cas.S
 * documents, and the kernel's trap path restarts both.  Restart is
 * sound here for the same two reasons: the store is the region's last
 * instruction, and nothing inside writes R1, so re-entering at the top
 * is indistinguishable from never having begun.  R11 is scratch and its
 * value does not survive the sequence.
 *
 * Preemption is deliberately not disabled.  The vector routine disables
 * it because its slow path inspects the owner and may block; a bare
 * acquire does not need it, because being preempted inside the sequence
 * restarts it and being preempted after the store is harmless — the
 * mutex is held by then.
 *
 * The owner test is `== 0`, which is the unowned adaptive mutex and
 * nothing else.  A spin mutex carries MUTEX_BIT_SPIN, and a mutex with
 * waiters carries MUTEX_BIT_WAITERS, so both fall to the vector routine.
 * MUTEX_BIT_NODEBUG would also make an unowned mutex non-zero, but it is
 * 0x00 unless LOCKDEBUG is set, and LOCKDEBUG is exactly when
 * kern_mutex.c undefines __HAVE_MUTEX_STUBS and this file compiles to
 * nothing — so the test is valid precisely when the stub exists.
 *
 * Curlwp lives in TP, so it is the value an acquire stores and the value
 * a release compares against.
 */

#include "opt_lockdebug.h"

#ifndef LOCKDEBUG

	.text

/*
 * void mutex_enter(kmutex_t *mtx)
 *   R1 = mtx
 */
	.balign	16
	.globl	mutex_enter
	.type	mutex_enter, @function

	/* Region markers — read by the kernel's trap path */
	.globl	_mutex_enter_ras_start
	.globl	_mutex_enter_ras_end

mutex_enter:
_mutex_enter_ras_start:
	ldw	r11, [r1]	/* r11 = mtx->mtx_owner */
	cmp	r11, 0
	bne	1f		/* owned, spin, or has waiters */
	stw	tp, [r1]	/* mtx_owner = curlwp */
_mutex_enter_ras_end:		/* one past the store, not on it */
	ret
1:
	b	mutex_vector_enter

	/*
	 * Growing a region changes what a restart redoes, and the trap
	 * path depends on the store staying last — a deliberate decision,
	 * not an edit to be made in passing.  The 16-byte size also keeps
	 * every sequence one aligned block, which is what lets the trap
	 * path identify them without a per-region test.
	 */
	.if (_mutex_enter_ras_end - _mutex_enter_ras_start) != 16
	.error "mutex_enter restartable region changed size"
	.endif

/*
 * void mutex_exit(kmutex_t *mtx)
 *   R1 = mtx
 */
	.balign	16
	.globl	mutex_exit
	.type	mutex_exit, @function

	.globl	_mutex_exit_ras_start
	.globl	_mutex_exit_ras_end

mutex_exit:
_mutex_exit_ras_start:
	ldw	r11, [r1]	/* r11 = mtx->mtx_owner */
	cmp	r11, tp
	bne	1f		/* not ours, spin, or has waiters */
	stw	zero, [r1]	/* mtx_owner = 0 */
_mutex_exit_ras_end:		/* one past the store, not on it */
	ret
1:
	b	mutex_vector_exit

	.if (_mutex_exit_ras_end - _mutex_exit_ras_start) != 16
	.error "mutex_exit restartable region changed size"
	.endif

#endif /* !LOCKDEBUG */
