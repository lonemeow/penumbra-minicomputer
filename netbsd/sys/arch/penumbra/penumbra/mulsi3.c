/*	$NetBSD$	*/

/*
 * Compiler runtime: 32-bit multiply.
 * Penumbra has no hardware MUL instruction — this is the
 * libcall target emitted by LLVM for i32 multiplications.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/types.h>

int32_t __mulsi3(int32_t, int32_t);

int32_t
__mulsi3(int32_t a, int32_t b)
{
	int32_t result = 0;
	uint32_t ua = (uint32_t)a;
	uint32_t ub = (uint32_t)b;

	while (ub) {
		if (ub & 1)
			result += ua;
		ua <<= 1;
		ub >>= 1;
	}
	return result;
}
