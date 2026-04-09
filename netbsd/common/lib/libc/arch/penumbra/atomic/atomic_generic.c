/*	$NetBSD$	*/

/*
 * Generic __atomic_* builtins for arbitrary-size types.
 *
 * These are the unsized versions (no _N suffix) that GCC/clang emit for
 * atomic operations on structs or types larger than the native word size.
 * They use a simple spinlock since Penumbra has no hardware LL/SC or
 * multi-word CAS.
 *
 * Clang treats __atomic_* as builtins and rejects redefinition, so we
 * define the functions with internal names and use asm labels to give
 * them the right symbol names in the object file.
 */

#include <string.h>
#include <stddef.h>
#include <stdbool.h>

/* Simple spinlock for generic atomics.  Adequate for single-threaded
 * userland (init); proper locking needs RAS or futex. */
static volatile int generic_lock;

static inline void
lock(void)
{
	while (generic_lock)
		;
	generic_lock = 1;
}

static inline void
unlock(void)
{
	generic_lock = 0;
}

/*
 * Use asm labels to set the ELF symbol name without triggering
 * clang's builtin redeclaration check.
 */

void
penumbra_atomic_load(unsigned int size, void *src, void *dest, int model)
    __asm("__atomic_load");

void
penumbra_atomic_load(unsigned int size, void *src, void *dest, int model)
{
	lock();
	memcpy(dest, src, size);
	unlock();
}

void
penumbra_atomic_store(unsigned int size, void *dest, void *src, int model)
    __asm("__atomic_store");

void
penumbra_atomic_store(unsigned int size, void *dest, void *src, int model)
{
	lock();
	memcpy(dest, src, size);
	unlock();
}

void
penumbra_atomic_exchange(unsigned int size, void *ptr, void *val,
    void *old, int model)
    __asm("__atomic_exchange");

void
penumbra_atomic_exchange(unsigned int size, void *ptr, void *val,
    void *old, int model)
{
	lock();
	memcpy(old, ptr, size);
	memcpy(ptr, val, size);
	unlock();
}

int
penumbra_atomic_compare_exchange(unsigned int size, void *ptr,
    void *expected, void *desired, int success_model, int failure_model)
    __asm("__atomic_compare_exchange");

int
penumbra_atomic_compare_exchange(unsigned int size, void *ptr,
    void *expected, void *desired, int success_model, int failure_model)
{
	int ret;

	lock();
	if (memcmp(ptr, expected, size) == 0) {
		memcpy(ptr, desired, size);
		ret = 1;
	} else {
		memcpy(expected, ptr, size);
		ret = 0;
	}
	unlock();
	return ret;
}

bool
penumbra_atomic_is_lock_free(size_t size, const volatile void *ptr)
    __asm("__atomic_is_lock_free");

bool
penumbra_atomic_is_lock_free(size_t size, const volatile void *ptr)
{
	(void)ptr;
	return (size <= 4 && (size & (size - 1)) == 0);
}
