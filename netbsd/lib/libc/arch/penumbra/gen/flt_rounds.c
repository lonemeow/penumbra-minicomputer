/*	$NetBSD$	*/

#include <sys/cdefs.h>

/*
 * Penumbra has no FPU — always round to nearest.
 * FLT_ROUNDS value 1 = round to nearest.
 *
 * When a hardware FPU is added, this must read the FPCSR
 * rounding mode and map it to the C FLT_ROUNDS values.
 */
#ifdef __PENUMBRA_HAS_FPU__
#error "Hardware FPU enabled but flt_rounds.c is still the soft-float stub — read FPCSR instead"
#endif

int __flt_rounds(void);

int
__flt_rounds(void)
{
	return 1;
}
