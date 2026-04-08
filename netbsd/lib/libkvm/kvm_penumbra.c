/*	$NetBSD$	*/

/*
 * Penumbra machine dependent routines for kvm.  Stub implementation.
 */

#include <sys/param.h>
#include <sys/exec.h>
#include <sys/types.h>

#include <uvm/uvm_extern.h>

#include <db.h>
#include <limits.h>
#include <kvm.h>
#include <stdlib.h>
#include <unistd.h>

#include "kvm_private.h"

#include <sys/kcore.h>
#include <machine/kcore.h>
#include <machine/vmparam.h>

void
_kvm_freevtop(kvm_t *kd)
{
	if (kd->vmst != 0)
		free(kd->vmst);
}

int
_kvm_initvtop(kvm_t *kd)
{
	return 0;
}

int
_kvm_kvatop(kvm_t *kd, vaddr_t va, paddr_t *pa)
{
	if (ISALIVE(kd)) {
		_kvm_err(kd, 0, "vatop called in live kernel!");
		return 0;
	}

	*pa = (u_long)~0UL;
	return 0;
}

off_t
_kvm_pa2off(kvm_t *kd, paddr_t pa)
{
	cpu_kcore_hdr_t	*cpu_kh;
	phys_ram_seg_t	*ram;
	off_t		off;
	void		*e;

	cpu_kh = kd->cpu_data;
	e = (char *)kd->cpu_data + kd->cpu_dsize;
	ram = (void *)((char *)(void *)cpu_kh + ALIGN(sizeof *cpu_kh));
	off = kd->dump_off;
	do {
		if (pa >= ram->start && (pa - ram->start) < ram->size) {
			return off + (pa - ram->start);
		}
		ram++;
		off += ram->size;
	} while ((void *)ram < e && ram->size);

	_kvm_err(kd, 0, "pa2off failed for pa %#" PRIxPADDR "\n", pa);
	return (off_t)-1;
}

int
_kvm_mdopen(kvm_t *kd)
{
	kd->min_uva = VM_MIN_ADDRESS;
	kd->max_uva = VM_MAXUSER_ADDRESS;
	return 0;
}
