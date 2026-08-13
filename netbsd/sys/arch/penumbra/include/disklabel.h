/*
 * Penumbra machine-dependent disk label definitions.
 */

#ifndef _PENUMBRA_DISKLABEL_H_
#define _PENUMBRA_DISKLABEL_H_

/*
 * The label lives in sector 1, clear of the MBR.  A 16-partition
 * disklabel is 408 bytes, so placing it at an offset within sector 0
 * would run into the MBR partition table at byte 446 and corrupt the
 * boot partition entry.
 */
#define LABELUSESMBR	1		/* MBR partitioning */
#define LABELSECTOR	1		/* sector containing label */
#define LABELOFFSET	0		/* offset of label in sector */
#define MAXPARTITIONS	16		/* number of partitions */
#define RAW_PART	2		/* raw partition: xx?c */

#if HAVE_NBTOOL_CONFIG_H
#include <nbinclude/sys/dkbad.h>
#else
#include <sys/dkbad.h>
#endif

struct cpu_disklabel {
#define __HAVE_DISKLABEL_DKBAD
	struct dkbad bad;
};

#endif /* !_PENUMBRA_DISKLABEL_H_ */
