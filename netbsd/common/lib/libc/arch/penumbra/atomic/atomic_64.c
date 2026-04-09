/*	$NetBSD$	*/

/*
 * 64-bit __atomic_*_8 builtins for Penumbra (32-bit, no 64-bit CAS).
 * Lock-based: uses same spinlock as atomic_generic.c.
 * Adequate for single-threaded userland; proper locking needs RAS/futex.
 *
 * Uses asm labels to avoid clang builtin redeclaration errors.
 */

#include <string.h>
#include <stdint.h>

/* Shared with atomic_generic.c — same translation unit isn't
 * guaranteed, so use a separate lock. */
static volatile int lock64;

static inline void
acq(void)
{
	while (lock64)
		;
	lock64 = 1;
}

static inline void
rel(void)
{
	lock64 = 0;
}

/* ── load ──────────────────────────────────────────────────────────── */

uint64_t
penumbra_atomic_load_8(volatile void *ptr, int model)
    __asm("__atomic_load_8");

uint64_t
penumbra_atomic_load_8(volatile void *ptr, int model)
{
	uint64_t val;
	acq();
	memcpy(&val, (void *)(uintptr_t)ptr, 8);
	rel();
	return val;
}

/* ── store ─────────────────────────────────────────────────────────── */

void
penumbra_atomic_store_8(volatile void *ptr, uint64_t val, int model)
    __asm("__atomic_store_8");

void
penumbra_atomic_store_8(volatile void *ptr, uint64_t val, int model)
{
	acq();
	memcpy((void *)(uintptr_t)ptr, &val, 8);
	rel();
}

/* ── exchange ──────────────────────────────────────────────────────── */

uint64_t
penumbra_atomic_exchange_8(volatile void *ptr, uint64_t val, int model)
    __asm("__atomic_exchange_8");

uint64_t
penumbra_atomic_exchange_8(volatile void *ptr, uint64_t val, int model)
{
	uint64_t old;
	acq();
	memcpy(&old, (void *)(uintptr_t)ptr, 8);
	memcpy((void *)(uintptr_t)ptr, &val, 8);
	rel();
	return old;
}

/* ── compare_exchange ──────────────────────────────────────────────── */

int
penumbra_atomic_compare_exchange_8(volatile void *ptr, void *expected,
    uint64_t desired, int success, int failure)
    __asm("__atomic_compare_exchange_8");

int
penumbra_atomic_compare_exchange_8(volatile void *ptr, void *expected,
    uint64_t desired, int success, int failure)
{
	int ret;
	acq();
	if (memcmp((void *)(uintptr_t)ptr, expected, 8) == 0) {
		memcpy((void *)(uintptr_t)ptr, &desired, 8);
		ret = 1;
	} else {
		memcpy(expected, (void *)(uintptr_t)ptr, 8);
		ret = 0;
	}
	rel();
	return ret;
}

/* ── fetch_add ─────────────────────────────────────────────────────── */

uint64_t
penumbra_atomic_fetch_add_8(volatile void *ptr, uint64_t val, int model)
    __asm("__atomic_fetch_add_8");

uint64_t
penumbra_atomic_fetch_add_8(volatile void *ptr, uint64_t val, int model)
{
	uint64_t old;
	acq();
	memcpy(&old, (void *)(uintptr_t)ptr, 8);
	uint64_t new_val = old + val;
	memcpy((void *)(uintptr_t)ptr, &new_val, 8);
	rel();
	return old;
}

/* ── fetch_sub ─────────────────────────────────────────────────────── */

uint64_t
penumbra_atomic_fetch_sub_8(volatile void *ptr, uint64_t val, int model)
    __asm("__atomic_fetch_sub_8");

uint64_t
penumbra_atomic_fetch_sub_8(volatile void *ptr, uint64_t val, int model)
{
	uint64_t old;
	acq();
	memcpy(&old, (void *)(uintptr_t)ptr, 8);
	uint64_t new_val = old - val;
	memcpy((void *)(uintptr_t)ptr, &new_val, 8);
	rel();
	return old;
}

/* ── fetch_and ─────────────────────────────────────────────────────── */

uint64_t
penumbra_atomic_fetch_and_8(volatile void *ptr, uint64_t val, int model)
    __asm("__atomic_fetch_and_8");

uint64_t
penumbra_atomic_fetch_and_8(volatile void *ptr, uint64_t val, int model)
{
	uint64_t old;
	acq();
	memcpy(&old, (void *)(uintptr_t)ptr, 8);
	uint64_t new_val = old & val;
	memcpy((void *)(uintptr_t)ptr, &new_val, 8);
	rel();
	return old;
}

/* ── fetch_or ──────────────────────────────────────────────────────── */

uint64_t
penumbra_atomic_fetch_or_8(volatile void *ptr, uint64_t val, int model)
    __asm("__atomic_fetch_or_8");

uint64_t
penumbra_atomic_fetch_or_8(volatile void *ptr, uint64_t val, int model)
{
	uint64_t old;
	acq();
	memcpy(&old, (void *)(uintptr_t)ptr, 8);
	uint64_t new_val = old | val;
	memcpy((void *)(uintptr_t)ptr, &new_val, 8);
	rel();
	return old;
}

/* ── fetch_xor ─────────────────────────────────────────────────────── */

uint64_t
penumbra_atomic_fetch_xor_8(volatile void *ptr, uint64_t val, int model)
    __asm("__atomic_fetch_xor_8");

uint64_t
penumbra_atomic_fetch_xor_8(volatile void *ptr, uint64_t val, int model)
{
	uint64_t old;
	acq();
	memcpy(&old, (void *)(uintptr_t)ptr, 8);
	uint64_t new_val = old ^ val;
	memcpy((void *)(uintptr_t)ptr, &new_val, 8);
	rel();
	return old;
}
