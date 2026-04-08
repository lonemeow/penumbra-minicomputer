/*	$NetBSD$	*/

#ifndef _PENUMBRA_KCORE_H_
#define _PENUMBRA_KCORE_H_

typedef struct cpu_kcore_hdr {
	uint32_t kh_misc[8];
	phys_ram_seg_t kh_ramsegs[0];
} cpu_kcore_hdr_t;

#endif /* _PENUMBRA_KCORE_H_ */
