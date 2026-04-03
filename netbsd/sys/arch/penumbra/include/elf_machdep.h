/*	$NetBSD$	*/

#ifndef _PENUMBRA_ELF_MACHDEP_H_
#define _PENUMBRA_ELF_MACHDEP_H_

/* Penumbra is 32-bit only */
#define KERN_ELFSIZE	32
#define ARCH_ELFSIZE	32

/* EM_PENUMBRA = 0xF0DA (matches LLVM backend) */
#define EM_PENUMBRA	0xF0DA

#define ELF32_MACHDEP_ID_CASES	\
		case EM_PENUMBRA: \
			break;

#define ELF32_MACHDEP_ID	EM_PENUMBRA

/* Penumbra relocations (matches LLVM ELF.h) */
#define R_PENUMBRA_NONE		0
#define R_PENUMBRA_32		1
#define R_PENUMBRA_BRANCH22	2
#define R_PENUMBRA_IMM16	3
#define R_PENUMBRA_MEMOFFSET16	4
#define R_PENUMBRA_LO16		5
#define R_PENUMBRA_HI16		6

#define R_TYPE(name)	__CONCAT(R_PENUMBRA_,name)

#endif /* _PENUMBRA_ELF_MACHDEP_H_ */
