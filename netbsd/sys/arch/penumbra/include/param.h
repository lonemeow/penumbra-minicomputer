/*	$NetBSD$	*/

#ifndef _PENUMBRA_PARAM_H_
#define _PENUMBRA_PARAM_H_

#define _MACHINE	penumbra
#define MACHINE		"penumbra"
#define _MACHINE_ARCH	penumbra
#define MACHINE_ARCH	"penumbra"
#define MID_MACHINE	0	/* not in NetBSD a.out midtable yet */

/*
 * Round p (pointer or byte index) up to a correctly-aligned value
 * for all data types.  The result is u_long and target-specific.
 */
/*
 * ALIGNBYTES and DEV_BSIZE are defined by sys/param.h from
 * __ALIGNBYTES (machine/cdefs.h) and DEV_BSHIFT respectively.
 * Don't redefine them here.
 */

/* Page size: 4 KB (matches Penumbra MMU) */
#define PGSHIFT		12
#define NBPG		(1 << PGSHIFT)
#define PGOFSET		(NBPG - 1)

#ifdef _KERNEL_OPT
#include "opt_param.h"
#endif

/*
 * u-space (kernel stack + PCB for each LWP).
 *
 * 4 pages (16 KB) — sized for -O0 + DIAGNOSTIC builds, where upstream
 * NetBSD init functions have large unoptimised stack frames
 * (e.g. sysctl_net_inet_tcp_setup2 at ~7.7 KB, sysctl_kern_setup at
 * ~5.9 KB).  Most other arches ship with 2 but compile at -O2 without
 * DIAGNOSTIC; hppa/ia64/newer-powerpc use 4 for similar reasons.
 *
 * TODO: add a guard page (cpu_uarea_alloc + pmap_kremove the redzone,
 * like x86's __HAVE_CPU_UAREA_ROUTINES path) so overflows trap
 * precisely instead of manifesting as double-faults in _trap_common.
 */
#define	UPAGES		4		/* pages of u-area */
#define	USPACE		(UPAGES * NBPG)	/* total size of u-area */

#ifndef MSGBUFSIZE
#define	MSGBUFSIZE	NBPG		/* default message buffer size */
#endif

/*
 * Constants related to network buffer management.
 * MCLBYTES must be no larger than NBPG (the software page size).
 */
#define	MSIZE		256		/* size of an mbuf */

#ifndef MCLSHIFT
#define	MCLSHIFT	11		/* convert bytes to m_buf clusters */
					/* 2K cluster can hold Ether frame */
#endif

#define	MCLBYTES	(1 << MCLSHIFT)	/* size of a m_buf cluster */

#define BLKDEV_IOSIZE	2048

#define MAXPHYS		(64 * 1024)

#if defined(_KERNEL) && !defined(_LOCORE)
/* Microsecond delay — stub, busy-loops for now */
#ifndef __HIDE_DELAY
void	delay(unsigned int);
#define	DELAY(n)	delay(n)
#endif
#endif /* _KERNEL && !_LOCORE */

/*
 * Minimum and maximum sizes of the kernel malloc arena in PAGE_SIZE-sized
 * logical pages.
 */
#define	NKMEMPAGES_MIN_DEFAULT	((4 * 1024 * 1024) >> PAGE_SHIFT)
#define	NKMEMPAGES_MAX_DEFAULT	((32 * 1024 * 1024) >> PAGE_SHIFT)

#endif /* _PENUMBRA_PARAM_H_ */
