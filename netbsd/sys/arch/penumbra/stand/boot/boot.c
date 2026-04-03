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

/* TODO(human): implement main() */
int
main(uint32_t bootdata)
{
	return 1;
}
