/*
 * boot.c — Penumbra stage 1 bootloader
 *
 * Loaded by the ROM from the partition gap into base RAM.
 * Receives R1 = physical pointer to Penumbra boot data.
 * Loads the stage 2 bootloader from the FAT32 boot partition.
 */

#include <lib/libsa/stand.h>
#include <lib/libsa/loadfile.h>

/* SD block device init (from libsa/sdblk.c) */
extern int sd_boot_init(uint32_t bootdata);

/* Boot data pointer, set by crt0 from R1 */
extern uint32_t boot_data;

static const char *boot2_paths[] = {
	"boot/boot2",
	"boot2",
	NULL
};

/*
 * devopen — called by libsa's open() to set up the device.
 *
 * For our single-device setup, this just points *file at the
 * filename portion and selects device 0 (SD).
 */
int
devopen(struct open_file *f, const char *fname, char **file)
{
	f->f_dev = &devsw[0];
	*file = (char *)fname;
	return 0;
}

/* libsa requires _rtt() for panic/exit */
void
_rtt(void)
{
	printf("Halting.\n");
	__asm volatile("break");
	for (;;)
		;
}

int
main(uint32_t bootdata)
{
	const char **path;
	int fd;

	printf("NetBSD/Penumbra boot\n\n");

	if (sd_boot_init(bootdata) != 0) {
		printf("SD init failed, halting.\n");
		_rtt();
	}

	for (path = boot2_paths; *path != NULL; path++) {
		fd = open(*path, 0);
		if (fd >= 0) {
			printf("Found: %s\n", *path);
			close(fd);
			break;
		}
	}

	if (*path == NULL) {
		printf("Stage 2 not found, halting.\n");
		_rtt();
	}

	/* TODO: loadfile() and jump to stage 2 */
	printf("Loading not yet implemented.\n");
	_rtt();
	return 1;
}
