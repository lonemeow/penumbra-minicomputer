/*	$NetBSD$	*/

/*
 * User/kernel copy functions — stubs.
 *
 * Real implementations need setjmp-based fault recovery (pcb_onfault)
 * so that a bad user pointer returns EFAULT instead of crashing.
 *
 * TODO: implement with proper fault recovery.
 */

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/systm.h>
#include <sys/errno.h>

int
copyin(const void *uaddr, void *kaddr, size_t len)
{
	/* TODO(stub) */
	__asm volatile("break");
	return EFAULT;
}

int
copyout(const void *kaddr, void *uaddr, size_t len)
{
	/* TODO(stub) */
	__asm volatile("break");
	return EFAULT;
}

int
copyinstr(const void *uaddr, void *kaddr, size_t len, size_t *done)
{
	/* TODO(stub) */
	__asm volatile("break");
	return EFAULT;
}
