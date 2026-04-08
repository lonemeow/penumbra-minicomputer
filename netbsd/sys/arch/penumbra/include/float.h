/*	$NetBSD$	*/

#ifndef _PENUMBRA_FLOAT_H_
#define _PENUMBRA_FLOAT_H_

#include <sys/cdefs.h>

#define LDBL_MANT_DIG	__LDBL_MANT_DIG__
#define LDBL_DIG	__LDBL_DIG__
#define LDBL_MIN_EXP	__LDBL_MIN_EXP__
#define LDBL_MIN_10_EXP	__LDBL_MIN_10_EXP__
#define LDBL_MAX_EXP	__LDBL_MAX_EXP__
#define LDBL_MAX_10_EXP	__LDBL_MAX_10_EXP__
#define LDBL_EPSILON	__LDBL_EPSILON__
#define LDBL_MIN	__LDBL_MIN__
#define LDBL_MAX	__LDBL_MAX__

#include <sys/float_ieee754.h>

#if (!defined(_ANSI_SOURCE) && !defined(_POSIX_C_SOURCE) \
	 && !defined(_XOPEN_SOURCE)) \
	|| (__STDC_VERSION__ - 0) >= 199901L \
	|| (_POSIX_C_SOURCE - 0) >= 200112L \
	|| ((_XOPEN_SOURCE  - 0) >= 600) \
	|| defined(_ISOC99_SOURCE) || defined(_NETBSD_SOURCE)
#define DECIMAL_DIG	__DECIMAL_DIG__
#endif

#endif /* !_PENUMBRA_FLOAT_H_ */
