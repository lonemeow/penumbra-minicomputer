/*	$NetBSD$	*/

/*-
 * Copyright (c) 2002, 2007 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jason R. Thorpe and Andrew Doran.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef _PENUMBRA_MUTEX_H_
#define _PENUMBRA_MUTEX_H_

#include <sys/types.h>

#ifndef __MUTEX_PRIVATE

struct kmutex {
	uintptr_t	mtx_pad1;
};

#else	/* __MUTEX_PRIVATE */

#include <machine/intr.h>

struct kmutex {
	union {
		/* Adaptive mutex */
		volatile uintptr_t	mtxa_owner;

		/* Spin mutex */
		struct {
			volatile uint8_t	mtxs_dummy;
			ipl_cookie_t		mtxs_ipl;
			__cpu_simple_lock_t	mtxs_lock;
			volatile uint8_t	mtxs_unused;
		} s;
	} u;
};

#define	mtx_owner		u.mtxa_owner
#define	mtx_ipl			u.s.mtxs_ipl
#define	mtx_lock		u.s.mtxs_lock

#define	__HAVE_SIMPLE_MUTEXES		1

/*
 * mutex_enter() and mutex_exit() come from lock_stubs.S, which handles
 * the uncontended adaptive case inline and branches to the C vector
 * routine for everything else.  __HAVE_SPIN_MUTEX_STUBS is unset
 * because no stub covers the spin variants, so kern_mutex.c aliases
 * mutex_spin_enter()/mutex_spin_exit() to the vector routines.
 */
#define	__HAVE_MUTEX_STUBS		1

/*
 * The spin-mutex lock byte is only ever reached with the mutex's IPL
 * already raised, because MUTEX_SPIN_SPLRAISE() runs before the try.
 * On a uniprocessor that leaves nothing able to touch the byte between
 * the test and the set: an interrupt at or below the mutex's IPL is
 * masked, and one above it may not take a lock of this IPL at all.
 * kern_mutex.c states the same invariant from the other side — without
 * MULTIPROCESSOR a failed try is MUTEX_ABORT("locking against myself"),
 * never contention — so the try cannot legitimately fail here.
 *
 * A plain read-modify-write therefore suffices, in place of the generic
 * macro's compare-and-swap, which has no hardware form on this ISA and
 * resolves to an out-of-line restartable sequence.  Only a kernel built
 * with DIAGNOSTIC, MULTIPROCESSOR or LOCKDEBUG touches the byte at all
 * (kern_mutex.c gates it on FULL).
 */
static __inline int
penumbra_mutex_spinbit_lock_try(__cpu_simple_lock_t *__lp)
{

	if (*__lp != __SIMPLELOCK_UNLOCKED)
		return 0;
	*__lp = __SIMPLELOCK_LOCKED;
	__insn_barrier();
	return 1;
}

#define	MUTEX_SPINBIT_LOCK_TRY(mtx)					\
	penumbra_mutex_spinbit_lock_try(&(mtx)->mtx_lock)

#endif	/* __MUTEX_PRIVATE */

#endif /* _PENUMBRA_MUTEX_H_ */
