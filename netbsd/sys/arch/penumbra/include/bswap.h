/*	$NetBSD$	*/

#ifndef _PENUMBRA_BSWAP_H_
#define _PENUMBRA_BSWAP_H_

#ifndef _LOCORE
#include <machine/byte_swap.h>
#endif

#define __BSWAP_RENAME
#include <sys/bswap.h>

#endif /* _PENUMBRA_BSWAP_H_ */
