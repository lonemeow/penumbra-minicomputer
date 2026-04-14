/*	$NetBSD$	*/

#include <machine/asm.h>

.globl	return_one, return_one_end

ENTRY_NP(return_one)
	li	r1, 1
	ret
return_one_end:
END(return_one)
