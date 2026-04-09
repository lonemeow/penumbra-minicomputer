/*	$NetBSD$	*/

/*
 * Penumbra has no FPU.  Provide minimal fenv.h to satisfy libc.
 * All rounding/exception operations are no-ops.
 */

#ifndef _PENUMBRA_FENV_H_
#define _PENUMBRA_FENV_H_

typedef int fenv_t;
typedef int fexcept_t;

#define	FE_INEXACT	0x01
#define	FE_UNDERFLOW	0x02
#define	FE_OVERFLOW	0x04
#define	FE_DIVBYZERO	0x08
#define	FE_INVALID	0x10

#define	FE_ALL_EXCEPT	0x1f

#define	FE_TONEAREST	0
#define	FE_TOWARDZERO	1
#define	FE_DOWNWARD	2
#define	FE_UPWARD	3

/*
 * fenv_t layout (pure software — no hardware FP register):
 *   bits  4:0  — exception flags (FE_*)
 *   bits  9:5  — exception mask
 *   bits 11:10 — rounding mode
 */
#define __FENV_GET_FLAGS(__envp)		((*(__envp)) & 0x1f)
#define __FENV_GET_MASK(__envp)		(((*(__envp)) >> 5) & 0x1f)
#define __FENV_GET_ROUND(__envp)	(((*(__envp)) >> 10) & 0x03)
#define __FENV_SET_FLAGS(__envp, __val) \
	(*(__envp) = (*(__envp) & ~0x1f) | ((__val) & 0x1f))
#define __FENV_SET_MASK(__envp, __val) \
	(*(__envp) = (*(__envp) & ~(0x1f << 5)) | (((__val) & 0x1f) << 5))
#define __FENV_SET_ROUND(__envp, __val) \
	(*(__envp) = (*(__envp) & ~(0x03 << 10)) | (((__val) & 0x03) << 10))

__BEGIN_DECLS

extern const fenv_t	__fe_dfl_env;
#define FE_DFL_ENV	(&__fe_dfl_env)

__END_DECLS

#endif /* _PENUMBRA_FENV_H_ */
