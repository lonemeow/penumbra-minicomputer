/*	$NetBSD$	*/

#ifndef _PENUMBRA_CPU_COUNTER_H_
#define _PENUMBRA_CPU_COUNTER_H_

#ifdef _KERNEL

#include <sys/types.h>
#include <machine/sysreg.h>

/*
 * Penumbra exposes a free-running 32-bit cycle counter at
 * SYSDEV_CPU / CPU_CYCLES.  It wraps every ~2.8 minutes at the
 * 25 MHz ULX3S clock; callers measuring longer intervals must sample
 * frequently enough to detect wrap, or use the MI timecounter.
 */

static __inline int
cpu_hascounter(void)
{
	return 1;
}

static __inline uint32_t
cpu_counter32(void)
{
	uint32_t v;
	__asm__ volatile("RDSYS %0, %1, %2"
	    : "=r"(v) : "i"(SYSDEV_CPU), "i"(CPU_CYCLES));
	return v;
}

static __inline uint64_t
cpu_counter(void)
{
	return (uint64_t)cpu_counter32();
}

#include <machine/cpu.h>

static __inline uint64_t
cpu_frequency(struct cpu_info *ci)
{
	return cpu_clock_freq_hz;
}

#endif /* _KERNEL */

#endif /* _PENUMBRA_CPU_COUNTER_H_ */
