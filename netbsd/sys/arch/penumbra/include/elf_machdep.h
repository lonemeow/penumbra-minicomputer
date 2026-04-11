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
#define ELF32_MACHDEP_ENDIANNESS	ELFDATA2LSB

/* No 64-bit ELF support — stubs for generic ELF code that uses both. */
#define ELF64_MACHDEP_ID_CASES		/* nothing */
#define ELF64_MACHDEP_ID		EM_NONE
#define ELF64_MACHDEP_ENDIANNESS	ELFDATA2LSB

/* Penumbra relocations (matches LLVM ELFRelocs/Penumbra.def) */
#define R_PENUMBRA_NONE			0
#define R_PENUMBRA_32			1
#define R_PENUMBRA_BRANCH22		2
#define R_PENUMBRA_IMM16		3
#define R_PENUMBRA_LO16			4
#define R_PENUMBRA_HI16			5
#define R_PENUMBRA_MEMOFFSET16_PCREL	6
#define R_PENUMBRA_IMM16_PCREL		7
#define R_PENUMBRA_RELATIVE		8
#define R_PENUMBRA_TLS_GD_LO16		9
#define R_PENUMBRA_TLS_GD_HI16		10
#define R_PENUMBRA_GLOB_DAT		11
#define R_PENUMBRA_JUMP_SLOT		12
#define R_PENUMBRA_TLS_TPOFF32		13
#define R_PENUMBRA_TLS_DTPMOD32		14
#define R_PENUMBRA_TLS_DTPOFF32		15
#define R_PENUMBRA_TLS_GD_PCREL		16
#define R_PENUMBRA_PC32			17

/* Size-qualified aliases for ld.elf_so R_TYPESZ() macro */
#define R_PENUMBRA_ADDR32		R_PENUMBRA_32

#define R_TYPE(name)	__CONCAT(R_PENUMBRA_,name)
#define R_TYPESZ(name)	__CONCAT(__CONCAT(R_PENUMBRA_,name),32)

#endif /* _PENUMBRA_ELF_MACHDEP_H_ */
