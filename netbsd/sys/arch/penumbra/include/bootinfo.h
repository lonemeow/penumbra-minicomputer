/*	$NetBSD$	*/

/*
 * Penumbra bootinfo — passed from bootloader to kernel via R1.
 *
 * Tagged list of variable-length entries, similar to x86/cobalt
 * bootinfo.  The bootloader translates Penumbra firmware boot data
 * (BTAG_*) into this OS-level format.
 *
 * Layout in memory:
 *   struct bootinfo header (magic + nentries)
 *   struct btinfo_common + payload  (entry 0)
 *   struct btinfo_common + payload  (entry 1)
 *   ...
 *
 * Entries are packed consecutively; walk by advancing `len` bytes.
 * All entries are 4-byte aligned.
 */

#ifndef _PENUMBRA_BOOTINFO_H_
#define _PENUMBRA_BOOTINFO_H_

#define BOOTINFO_MAGIC		0x50454E49	/* "PENI" (PENumbra Info) */
#define BOOTINFO_MAXSIZE	4096

/*
 * Common header for each bootinfo entry.
 */
struct btinfo_common {
	uint32_t len;		/* total length including this header */
	uint32_t type;		/* BTINFO_* tag */
};

/*
 * Tag types.
 */
#define BTINFO_MEMORY		1	/* physical RAM region */
#define BTINFO_CONSOLE		2	/* console device */
#define BTINFO_BOOTPATH		3	/* boot device */
#define BTINFO_SYMTAB		4	/* kernel symbol table */
#define BTINFO_KERNBASE		5	/* kernel physical load address */
#define BTINFO_DEVICE		6	/* discovered peripheral device */

/*
 * BTINFO_MEMORY — one entry per physical RAM region.
 * There may be multiple (base RAM, expansion, etc.).
 */
struct btinfo_memory {
	struct btinfo_common common;
	uint32_t base;		/* physical base address */
	uint32_t size;		/* size in bytes */
};

/*
 * BTINFO_CONSOLE — primary console device.
 */
struct btinfo_console {
	struct btinfo_common common;
	uint32_t addr;		/* MMIO base address */
	uint32_t speed;		/* baud rate (0 = unknown) */
	char devname[16];	/* e.g. "com0" */
};

/*
 * BTINFO_BOOTPATH — device the kernel was loaded from.
 * Gives the kernel enough info to find root.
 */
struct btinfo_bootpath {
	struct btinfo_common common;
	uint32_t bus_addr;	/* SPI controller MMIO base */
	uint32_t cs;		/* chip-select pin */
	uint32_t partition;	/* partition number (1-based, 0 = raw) */
	char devname[16];	/* e.g. "sd0" */
};

/*
 * BTINFO_SYMTAB — kernel symbol table location.
 * Populated by loadfile() marks after loading the kernel.
 */
struct btinfo_symtab {
	struct btinfo_common common;
	uint32_t nsym;		/* number of symbols */
	uint32_t ssym;		/* start of symbol table (virtual) */
	uint32_t esym;		/* end of symbol table (virtual) */
};

/*
 * BTINFO_KERNBASE — where the kernel was physically loaded.
 * The kernel is linked at KERNEL_TEXT_BASE (0x8001_0000) but
 * loaded at a different physical address.  locore.S needs this
 * to set up the initial virtual-to-physical TLB mappings.
 */
struct btinfo_kernbase {
	struct btinfo_common common;
	uint32_t phys_base;	/* physical address of kernel text */
	uint32_t kern_start;	/* virtual start (from ELF, == link addr) */
	uint32_t kern_end;	/* virtual end (text + data + bss) */
};

/*
 * BTINFO_DEVICE — a peripheral device discovered by ROM autoconfig.
 * The kernel cannot re-run autoconfig (it would reset the bus),
 * so every device must be passed through here.
 */
struct btinfo_device {
	struct btinfo_common common;
	uint32_t cls;		/* device class (ACFG_CLASS_*) */
	uint32_t addr;		/* MMIO base address */
	uint32_t size;		/* MMIO region size */
	uint32_t id;		/* device ID from autoconfig */
	char name[16];		/* device name */
};

/*
 * Top-level bootinfo header.  R1 points here.
 * Kernel copies `size` bytes into BSS on entry, then the
 * bootloader's original buffer can be reclaimed.
 */
struct bootinfo {
	uint32_t magic;		/* BOOTINFO_MAGIC */
	uint32_t size;		/* total size in bytes (header + entries) */
	uint32_t nentries;	/* number of btinfo entries following */
	/* struct btinfo_common entries[] follows */
};

/*
 * Device classes — shared between ROM, bootloader, and kernel.
 * These values are baked into autoconfig hardware registers and
 * boot data, so they must never change.
 */
#define ACFG_CLASS_UNKNOWN	0
#define ACFG_CLASS_MEMORY	1
#define ACFG_CLASS_UART		2
#define ACFG_CLASS_SPI		3
#define ACFG_CLASS_SD		4

#ifdef _KERNEL
void	*lookup_bootinfo(unsigned int);
void	*lookup_bootinfo_n(unsigned int, int);
#endif

#endif /* _PENUMBRA_BOOTINFO_H_ */
