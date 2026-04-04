/*
 * Penumbra machine-dependent disk label definitions.
 */

#ifndef _PENUMBRA_DISKLABEL_H_
#define _PENUMBRA_DISKLABEL_H_

#define LABELUSESMBR	1		/* MBR partitioning */
#define LABELSECTOR	0		/* sector containing label */
#define LABELOFFSET	64		/* offset of label in sector */
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
