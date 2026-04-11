/*	$NetBSD$	*/

#ifndef _PENUMBRA_TYPES_H_
#define _PENUMBRA_TYPES_H_

#include <sys/cdefs.h>
#include <sys/featuretest.h>
#include <machine/int_types.h>

/* Penumbra is ILP32: 32-bit registers, addresses, pointers */
typedef __int32_t		__register_t;
typedef unsigned char		__cpu_simple_lock_nv_t;

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
	register_t val[9];	/* callee-saved R5-R10, R12(TP), R13(LR), R14(SP) */
} label_t;

/* Indices into label_t.val[] — match setjmp/longjmp/cpu_switchto order */
#define	_JB_R5		0
#define	_JB_R6		1
#define	_JB_R7		2
#define	_JB_R8		3
#define	_JB_R9		4
#define	_JB_R10		5
#define	_JB_R12		6	/* TP */
#define	_JB_R13		7	/* LR */
#define	_JB_R14		8	/* SP */

#endif /* _KERNEL || _KMEMUSER || _KERNTYPES || _STANDALONE */

#define __SIMPLELOCK_LOCKED	1
#define __SIMPLELOCK_UNLOCKED	0

#define __HAVE_COMMON___TLS_GET_ADDR
#define __HAVE_CPU_LWP_SETPRIVATE
#define __HAVE_RAS
#define __HAVE_SYSCALL_INTERN
#define __HAVE_TLS_VARIANT_I
#define __HAVE_NEW_STYLE_BUS_H

#endif /* _PENUMBRA_TYPES_H_ */
