/*
 * conf.c — Device and filesystem configuration for stage 1 bootloader
 *
 * Wires the SD block device and DOS (FAT32) filesystem into libsa.
 * Single device, single filesystem — keeps things simple.
 */

#include <lib/libsa/stand.h>
#include <lib/libsa/dosfs.h>

/* SD block device operations (from libsa/sdblk.c) */
extern int sdstrategy(void *, int, daddr_t, size_t, void *, size_t *);
extern int sdopen(struct open_file *, ...);
extern int sdclose(struct open_file *);
extern int sdioctl(struct open_file *, u_long, void *);

struct devsw devsw[] = {
	{
		.dv_name = "sd",
		.dv_strategy = sdstrategy,
		.dv_open = sdopen,
		.dv_close = sdclose,
		.dv_ioctl = sdioctl,
	},
};
int ndevs = sizeof(devsw) / sizeof(devsw[0]);

struct fs_ops file_system[] = {
	FS_OPS(dosfs),
};
int nfsys = sizeof(file_system) / sizeof(file_system[0]);
