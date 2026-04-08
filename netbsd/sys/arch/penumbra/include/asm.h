/*	$NetBSD$	*/

/*
 * Penumbra machine-dependent assembly macros.
 *
 * ILP32 only — no LP64 polymorphism needed.
 * Penumbra ABI: R13=LR, R14=SP, R5–R10 callee-saved,
 * R1–R4 args/scratch, R0=zero.
 */

#ifndef _PENUMBRA_ASM_H
#define _PENUMBRA_ASM_H

#define	_C_LABEL(x)	x

#define	__CONCAT(x,y)	x ## y
#define	__STRING(x)	#x

/*
 * No profiling support yet.
 */
#ifdef GPROF
#define	_PROF_PROLOGUE	/* not implemented */
#else
#define	_PROF_PROLOGUE
#endif

#ifdef __PIC__
#define	PLT(x)	x	/* no PLT on Penumbra — PC-relative PIC */
#else
#define	PLT(x)	x
#endif

/*
 * WEAK_ALIAS: create a weak alias.
 */
#define	WEAK_ALIAS(alias,sym)						\
	.weak alias;							\
	alias = sym

/*
 * STRONG_ALIAS: create a strong alias.
 */
#define	STRONG_ALIAS(alias,sym)						\
	.globl alias;							\
	alias = sym

/*
 * WARN_REFERENCES: create a warning if the specified symbol is referenced.
 */
#define	WARN_REFERENCES(sym,msg)					\
	.pushsection __CONCAT(.gnu.warning.,sym);			\
	.ascii msg;							\
	.popsection

#define	_ENTRY(x)			\
	.globl	_C_LABEL(x);		\
	.type	_C_LABEL(x), @function;	\
	_C_LABEL(x):

#define	ENTRY_NP(x)	.text; .p2align 2; _ENTRY(x)
#define	ENTRY(x)	ENTRY_NP(x); _PROF_PROLOGUE
#define	END(x)		.size _C_LABEL(x), . - _C_LABEL(x)

/*
 * Register size and load/store macros.
 * Penumbra is ILP32 — all registers are 32-bit.
 */
#define	SZREG	4

#define	REG_L	LDW
#define	REG_S	STW
#define	INT_L	LDW
#define	INT_S	STW
#define	PTR_L	LDW
#define	PTR_S	STW

/*
 * Standard call frame for leaf wrappers (e.g. cerror, syscall stubs).
 *
 *     [SP+4]  saved R13 (LR)
 *     [SP+0]  scratch / saved R1
 *
 * 8 bytes, 4-byte aligned (Penumbra has no stricter requirement).
 */
#define	CALLFRAME_SIZ	8
#define	CALLFRAME_S0	0	/* scratch slot (errno value) */
#define	CALLFRAME_LR	4	/* saved LR (R13) */

#define	__RCSID(x)	.pushsection ".ident","MS",@progbits,1;		\
			.asciz x;					\
			.popsection
#define	RCSID(name)	__RCSID(name)

#endif /* _PENUMBRA_ASM_H */
