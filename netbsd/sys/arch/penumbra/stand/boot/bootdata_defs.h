/*
 * bootdata_defs.h — Penumbra firmware boot data structure definitions.
 *
 * Shared between bootloader sources (boot.c, sdblk.c).
 * Must match hw/rom/bootdata.h and hw/rom/penumbra.h constants.
 */

#ifndef _BOOTDATA_DEFS_H_
#define _BOOTDATA_DEFS_H_

#include <sys/types.h>

/* Boot data header */
#define BOOTDATA_MAGIC		0x50454E42	/* "PENB" */

struct bootdata_hdr {
	uint32_t magic;
	uint32_t version;
	uint32_t total_size;
};

/* Common tag header */
struct btag_hdr {
	uint32_t type;
	uint32_t size;		/* total entry size including header */
};

/* Tag types */
#define BTAG_END		0
#define BTAG_DEVICE		2
#define BTAG_CONSOLE		3
#define BTAG_BOOTDEV		4

/* Device classes (must match hw/rom/penumbra.h ACFG_CLASS_*) */
#define ACFG_CLASS_UNKNOWN	0
#define ACFG_CLASS_MEMORY	1
#define ACFG_CLASS_UART		2
#define ACFG_CLASS_SPI		3
#define ACFG_CLASS_SD		4

/* BTAG_DEVICE payload */
struct btag_device {
	struct btag_hdr hdr;
	uint32_t cls;
	uint32_t base;
	uint32_t dev_size;
	uint32_t id;
	char     name[16];
};

/* BTAG_CONSOLE payload */
struct btag_console {
	struct btag_hdr hdr;
	uint32_t dev_nth;
};

/* BTAG_BOOTDEV payload */
struct btag_bootdev {
	struct btag_hdr hdr;
	uint32_t dev_nth;
	uint32_t cs;
	uint32_t partition;
};

#endif /* _BOOTDATA_DEFS_H_ */
