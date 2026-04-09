/*	$NetBSD$	*/

#ifndef _PENUMBRA_LOCK_H_
#define _PENUMBRA_LOCK_H_

/*
 * Machine-dependent spin lock primitives.
 *
 * __cpu_simple_lock_t is typedef'd in <sys/types.h> as:
 *   typedef volatile __cpu_simple_lock_nv_t __cpu_simple_lock_t;
 *
 * Penumbra is uniprocessor (no SMP), so simple locks can be
 * implemented by disabling interrupts.  When SMP is added,
 * these will need atomic (LL/SC or CAS) implementations.
 */

static __inline int
__SIMPLELOCK_LOCKED_P(const __cpu_simple_lock_t *lp)
{
	return *lp != __SIMPLELOCK_UNLOCKED;
}

static __inline int
__SIMPLELOCK_UNLOCKED_P(const __cpu_simple_lock_t *lp)
{
	return *lp == __SIMPLELOCK_UNLOCKED;
}

static __inline void
__cpu_simple_lock_init(__cpu_simple_lock_t *lp)
{
	*lp = __SIMPLELOCK_UNLOCKED;
	__insn_barrier();
}

static __inline void
__cpu_simple_lock_clear(__cpu_simple_lock_t *lp)
{
	*lp = __SIMPLELOCK_UNLOCKED;
	__insn_barrier();
}

static __inline void
__cpu_simple_lock_set(__cpu_simple_lock_t *lp)
{
	*lp = __SIMPLELOCK_LOCKED;
	__insn_barrier();
}

static __inline void
__cpu_simple_lock(__cpu_simple_lock_t *lp)
{
	*lp = __SIMPLELOCK_LOCKED;
	__insn_barrier();
}

static __inline int
__cpu_simple_lock_try(__cpu_simple_lock_t *lp)
{
	/*
	 * Use atomic CAS so this is safe under preemptive scheduling.
	 * In kernel context the CAS uses DI/EI; in userland it uses
	 * the libc atomic_cas_32 (currently also DI/EI via syscall,
	 * future: RAS).
	 */
	return __sync_bool_compare_and_swap(lp, __SIMPLELOCK_UNLOCKED,
	    __SIMPLELOCK_LOCKED);
}

static __inline void
__cpu_simple_unlock(__cpu_simple_lock_t *lp)
{
	__insn_barrier();
	*lp = __SIMPLELOCK_UNLOCKED;
}

#endif /* _PENUMBRA_LOCK_H_ */
