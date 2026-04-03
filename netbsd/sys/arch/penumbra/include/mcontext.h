/*	$NetBSD$	*/

#ifndef _PENUMBRA_MCONTEXT_H_
#define _PENUMBRA_MCONTEXT_H_

/*
 * Machine-dependent context for ucontext.
 * Minimal stub — full definition needed for kernel port.
 */

#define _NGREG	18	/* R0-R15 + SR + PC */

typedef int		__greg_t;
typedef __greg_t	__gregset_t[_NGREG];

#define _REG_R0		0
#define _REG_R15	15
#define _REG_PC		16
#define _REG_SR		17

typedef struct {
	__gregset_t	__gregs;
} mcontext_t;

#define _UC_MACHINE_SP(uc)	((uc)->uc_mcontext.__gregs[14])	/* R14=SP */
#define _UC_MACHINE_FP(uc)	0	/* no dedicated FP register */
#define _UC_MACHINE_PC(uc)	((uc)->uc_mcontext.__gregs[_REG_PC])
#define _UC_MACHINE_INTRV(uc)	((uc)->uc_mcontext.__gregs[1])	/* R1=retval */
#define _UC_MACHINE_SET_PC(uc, v) ((uc)->uc_mcontext.__gregs[_REG_PC] = (v))

#endif /* _PENUMBRA_MCONTEXT_H_ */
