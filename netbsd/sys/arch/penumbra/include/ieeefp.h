/*	$NetBSD$	*/

#ifndef _PENUMBRA_IEEEFP_H_
#define _PENUMBRA_IEEEFP_H_

#include <sys/featuretest.h>

#if defined(_NETBSD_SOURCE) || defined(_ISOC99_SOURCE)

#include <penumbra/fenv.h>

#if !defined(_ISOC99_SOURCE)

typedef int fp_except;

#define	FP_X_INV	FE_INVALID
#define	FP_X_DZ		FE_DIVBYZERO
#define	FP_X_OFL	FE_OVERFLOW
#define	FP_X_UFL	FE_UNDERFLOW
#define	FP_X_IMP	FE_INEXACT

typedef enum {
    FP_RN=FE_TONEAREST,
    FP_RP=FE_UPWARD,
    FP_RM=FE_DOWNWARD,
    FP_RZ=FE_TOWARDZERO
} fp_rnd;

#endif /* !_ISOC99_SOURCE */

#endif /* _NETBSD_SOURCE || _ISOC99_SOURCE */

#endif /* _PENUMBRA_IEEEFP_H_ */
