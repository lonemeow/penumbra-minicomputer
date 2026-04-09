/*	$NetBSD$	*/

/*
 * Compiler runtime: 32-bit multiply.
 * Penumbra has no hardware MUL instruction — this is the
 * libcall target emitted by LLVM for i32 multiplications.
 *
 */

#include <sys/cdefs.h>
__RCSID("$NetBSD$");

#include <sys/types.h>

int32_t __mulsi3(int32_t, int32_t);

int32_t
__mulsi3(int32_t a, int32_t b)
{
	u_int32_t r = 0;
	u_int32_t ua = a, ub = b;
	while (ua)
	{
		if (ua & 1)
			r += ub;
		ua >>= 1;
		ub <<= 1;
	}
	return r;
}
