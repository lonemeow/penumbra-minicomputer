/*	$NetBSD$	*/

#ifndef _PENUMBRA_MCONTEXT_H_
#define _PENUMBRA_MCONTEXT_H_

/*
 * Machine-dependent context for ucontext.
 *
 * __gregs[0..15] = R0–R15 (R15 is the program counter).
 * __gregs[16]    = SR (status register).
 *
 * When filled from a trapframe, __gregs[_REG_PC] holds EPC
 * (the saved PC), not the trap-vector value of R15.
 */

#define _NGREG	17	/* R0-R15 + SR */

typedef int		__greg_t;
typedef __greg_t	__gregset_t[_NGREG];

#define _REG_R0		0
#define _REG_R15	15
#define _REG_PC		15	/* R15 is the program counter */
#define _REG_SP		14	/* R14 is the stack pointer */
#define _REG_LR		13	/* R13 is the link register */
#define _REG_SR		16

typedef struct {
	__gregset_t	__gregs;
} mcontext_t;

#define _UC_MACHINE_SP(uc)	((uc)->uc_mcontext.__gregs[_REG_SP])
#define _UC_MACHINE_FP(uc)	0	/* no dedicated FP register */
#define _UC_MACHINE_PC(uc)	((uc)->uc_mcontext.__gregs[_REG_PC])
#define _UC_MACHINE_INTRV(uc)	((uc)->uc_mcontext.__gregs[1])	/* R1=retval */
#define _UC_MACHINE_SET_PC(uc, v) ((uc)->uc_mcontext.__gregs[_REG_PC] = (v))

/*
 * TLS support — Variant 1, no TCB gap (like RISC-V).
 * TP (R12) points past tls_tcb; tcb is at negative offset from TP.
 */
#if defined(_RTLD_SOURCE) || defined(_LIBC_SOURCE) || \
    defined(__LIBPTHREAD_SOURCE__)

#include <sys/tls.h>

#define	TLS_TP_OFFSET	0x0
#define	TLS_DTV_OFFSET	0x800
__CTASSERT(TLS_TP_OFFSET + sizeof(struct tls_tcb) < 0x800);

static __inline void *
__lwp_getprivate_fast(void)
{
	void *__tp;
	__asm("mov %0, r12" : "=r"(__tp));
	return __tp;
}

static __inline void *
__lwp_gettcb_fast(void)
{
	void *__tcb;
	__asm __volatile(
		"add %[__tcb], r12, %[__offset]"
	    :	[__tcb] "=r" (__tcb)
	    :	[__offset] "i" (-(TLS_TP_OFFSET + sizeof(struct tls_tcb))));
	return __tcb;
}

static __inline void
__lwp_settcb(void *__tcb)
{
	__asm __volatile(
		"add r12, %[__tcb], %[__offset]"
	    :
	    :	[__tcb] "r" (__tcb),
		[__offset] "i" (TLS_TP_OFFSET + sizeof(struct tls_tcb)));
}
#endif /* _RTLD_SOURCE || _LIBC_SOURCE || __LIBPTHREAD_SOURCE__ */

#endif /* _PENUMBRA_MCONTEXT_H_ */
