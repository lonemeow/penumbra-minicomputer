/*	$NetBSD$	*/

#ifndef _PENUMBRA_TYPES_H_
#define _PENUMBRA_TYPES_H_

#include <sys/cdefs.h>
#include <sys/featuretest.h>
#include <machine/int_types.h>

/* Penumbra is ILP32: 32-bit registers, addresses, pointers */
typedef __int32_t		__register_t;
typedef unsigned int		__cpu_simple_lock_nv_t;

typedef __uint32_t		__vaddr_t;

#if defined(_KERNEL) || defined(_KMEMUSER) || defined(_KERNTYPES) || defined(_STANDALONE)
typedef __uint32_t	paddr_t;
typedef __uint32_t	psize_t;
#define PRIxPADDR	PRIx32
#define PRIxPSIZE	PRIx32
#define PRIdPSIZE	PRId32
#define PRIuPSIZE	PRIu32

typedef __uint32_t	vaddr_t;
typedef __uint32_t	vsize_t;
#define PRIxVADDR	PRIx32
#define PRIxVSIZE	PRIx32
#define PRIdVSIZE	PRId32
#define PRIuVSIZE	PRIu32

typedef vaddr_t		vm_offset_t;
typedef vsize_t		vm_size_t;

typedef __register_t	register_t;

typedef struct label_t {
	register_t val[12];	/* callee-saved R5-R10, R12(TP), R13(LR), R14(SP), R15(PC), SR, filler */
} label_t;

#endif /* _KERNEL || _KMEMUSER || _KERNTYPES || _STANDALONE */

#define __SIMPLELOCK_LOCKED	1
#define __SIMPLELOCK_UNLOCKED	0

#define __HAVE_COMMON___TLS_GET_ADDR
#define __HAVE_SYSCALL_INTERN
#define __HAVE_TLS_VARIANT_I
#define __HAVE_NEW_STYLE_BUS_H

#endif /* _PENUMBRA_TYPES_H_ */
