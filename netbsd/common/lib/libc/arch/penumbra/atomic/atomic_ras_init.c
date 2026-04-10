/*	$NetBSD$	*/

/*
 * Register the atomic CAS RAS (Restartable Atomic Sequence) with the
 * kernel.  Must run before any atomic operation.  Uses .init_array
 * (constructor priority 101 — after CRT init, before general ctors).
 */

#include <sys/types.h>
#include <sys/ras.h>

extern char _atomic_cas_32_ras_start[];
extern char _atomic_cas_32_ras_end[];

static void
__atomic_ras_init(void)
{

	rasctl(_atomic_cas_32_ras_start,
	    (size_t)(_atomic_cas_32_ras_end - _atomic_cas_32_ras_start),
	    RAS_INSTALL);
}

__attribute__((section(".init_array"), used))
static void (*__atomic_ras_init_fn)(void) = __atomic_ras_init;
