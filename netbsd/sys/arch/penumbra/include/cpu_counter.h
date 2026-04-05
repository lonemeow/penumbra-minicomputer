/*	$NetBSD$	*/

#ifndef _PENUMBRA_CPU_COUNTER_H_
#define _PENUMBRA_CPU_COUNTER_H_

/*
 * CPU cycle counter — Penumbra does not yet have a hardware
 * cycle counter.  Stub returns false for cpu_hascounter().
 */

#ifdef _KERNEL

static __inline int
cpu_hascounter(void)
{
	return 0;	/* no hardware cycle counter yet */
}

static __inline uint32_t
cpu_counter(void)
{
	return 0;
}

static __inline uint32_t
cpu_counter32(void)
{
	return 0;
}

static __inline uint64_t
cpu_frequency(struct cpu_info *ci)
{
	return 0;
}

#endif /* _KERNEL */

#endif /* _PENUMBRA_CPU_COUNTER_H_ */
