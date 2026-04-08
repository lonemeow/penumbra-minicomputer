/*	$NetBSD$	*/

/*
 * Penumbra uses .init_array/.fini_array (HAVE_INITFINI_ARRAY),
 * so no .init/.fini prologues are needed here.
 */

#include <machine/asm.h>

RCSID("$NetBSD$")

#include "sysident.S"
