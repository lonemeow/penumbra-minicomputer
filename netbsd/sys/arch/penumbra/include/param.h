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

#define BLKDEV_IOSIZE	2048

#define MAXPHYS		(64 * 1024)

#endif /* _PENUMBRA_PARAM_H_ */
