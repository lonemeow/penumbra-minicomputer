/*	$NetBSD$	*/

#ifndef _PENUMBRA_BYTE_SWAP_H_
#define _PENUMBRA_BYTE_SWAP_H_

#ifdef __GNUC__
#include <sys/cdefs.h>
#include <sys/stdint.h>
__BEGIN_DECLS

#define	__BYTE_SWAP_U16_VARIABLE __byte_swap_u16_variable
static __inline uint16_t
__byte_swap_u16_variable(uint16_t v)
{
	return (uint16_t)((v >> 8) | (v << 8));
}

#define	__BYTE_SWAP_U32_VARIABLE __byte_swap_u32_variable
static __inline uint32_t
__byte_swap_u32_variable(uint32_t v)
{
	return ((v >> 24) & 0x000000ff) |
	       ((v >>  8) & 0x0000ff00) |
	       ((v <<  8) & 0x00ff0000) |
	       ((v << 24) & 0xff000000);
}

__END_DECLS
#endif

#endif /* _PENUMBRA_BYTE_SWAP_H_ */
