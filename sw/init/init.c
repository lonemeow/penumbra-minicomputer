/*
 * Minimal /sbin/init for Penumbra — first userland program.
 *
 * This is a diagnostic tool: if the kernel can exec this and we see
 * "Hello from userland" on the console, the entire exec/syscall/VM
 * path is working.  Once that's validated, this gets replaced by
 * real NetBSD init.
 *
 * Syscall convention (Penumbra ABI):
 *   R1 = syscall number
 *   R2–R4 = arguments
 *   SYSCALL instruction traps to kernel
 *   Return value in R1
 *
 * Syscall numbers from <sys/syscall.h>:
 *   SYS_exit  = 1
 *   SYS_write = 4
 */

#include <stdint.h>

static int penumbra_syscall(uint32_t num, uint32_t arg1, uint32_t arg2, uint32_t arg3)
{
	register uint32_t r1 __asm__("r1") = num;
	register uint32_t r2 __asm__("r2") = arg1;
	register uint32_t r3 __asm__("r3") = arg2;
	register uint32_t r4 __asm__("r4") = arg3;

	__asm__ volatile (
		"SYSCALL"
		: "+r" (r1)
		: "r" (r2), "r" (r3), "r" (r4)
		: "memory", "cc"
	);

	return (int)r1;
}

static void sys_write(int fd, const char *buf, int len)
{
	penumbra_syscall(/*SYS_write*/ 4, fd, (uint32_t)buf, len);
}

static void sys_exit(int status) __attribute__((noreturn));
static void sys_exit(int status)
{
	penumbra_syscall(/*SYS_exit*/ 1, status, 0, 0);
	__builtin_unreachable();
}

void _start(void)
{
	static const char msg[] = "Hello from userland\n";
	sys_write(1, msg, sizeof(msg) - 1);
	sys_exit(0);
}
