/*	$NetBSD$	*/

/*
 * Register the atomic CAS restartable sequence with the kernel.  Must
 * run before any atomic operation.
 *
 * This is libc's own atomic initialization hook, called from
 * _libc_init(), rather than a constructor of this file's own: nothing
 * references an object that only contributes to .init_array, so a
 * static link leaves it out and the program runs with an unregistered
 * sequence.  Ports whose atomics need setup (arm, hppa, sparc) hook in
 * the same place.
 */

#include <sys/types.h>
#include <sys/cdefs.h>
#include <sys/ras.h>

extern char _atomic_cas_32_ras_start[];
extern char _atomic_cas_32_ras_end[];

void	__libc_atomic_init(void) __attribute__ ((visibility("hidden")));

void __section(".text.startup")
__libc_atomic_init(void)
{

	rasctl(_atomic_cas_32_ras_start,
	    (size_t)(_atomic_cas_32_ras_end - _atomic_cas_32_ras_start),
	    RAS_INSTALL);
}
