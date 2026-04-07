/*	$NetBSD$	*/

/*
 * Penumbra Bus ("pbbus") — attachment arguments.
 *
 * Each child device on the Penumbra Bus receives this structure
 * during autoconf.  The fields come from BTINFO_DEVICE entries
 * built by the ROM and passed through the bootloader.
 *
 * Device classes are ACFG_CLASS_* from <machine/bootinfo.h> —
 * shared between ROM, bootloader, and kernel.
 */

#ifndef _PENUMBRA_PBBUS_H_
#define _PENUMBRA_PBBUS_H_

struct pbbus_attach_args {
	bus_space_tag_t	pb_iot;		/* bus space tag (unused, 0) */
	bus_addr_t	pb_addr;	/* MMIO base (physical) */
	bus_size_t	pb_size;	/* MMIO region size */
	uint32_t	pb_class;	/* ACFG_CLASS_* */
	uint32_t	pb_id;		/* device ID from autoconfig */
	const char	*pb_name;	/* device name from ROM */
};

#endif /* _PENUMBRA_PBBUS_H_ */
