/*	$NetBSD$	*/

/*
 * Penumbra syscall macros for libc.
 *
 * Syscall ABI:
 *   R1 = syscall number (in), return value or errno (out)
 *   R2–R4 = arguments 1–3
 *   SYSCALL instruction traps to kernel
 *   On return: SR.C = 0 → success (R1 = retval)
 *              SR.C = 1 → error   (R1 = errno)
 */

#include <sys/syscall.h>
#include <machine/asm.h>

/*
 * SYSTRAP(x) — load SYS_##x into R1 and execute SYSCALL.
 * JUMP_TO_CERROR() — branch to __cerror if carry set (error).
 * PSEUDO(x,y) — complete syscall wrapper with error handling.
 * PSEUDO_NOERROR(x,y) — syscall wrapper for calls that cannot fail.
 * RSYSCALL/RSYSCALL_NOERROR/WSYSCALL — convenience wrappers.
 */

#define SYSTRAP(x) lli r1, SYS_##x; syscall

#define JUMP_TO_CERROR() bcs __cerror

#define PSEUDO(x, y)  \
    ENTRY(x);         \
    SYSTRAP(y);       \
    JUMP_TO_CERROR(); \
    ret;              \
    END(x)

#define PSEUDO_NOERROR(x, y) \
    ENTRY(x);                \
    SYSTRAP(y);              \
    ret;                     \
    END(x)

#define RSYSCALL_NOERROR(x) PSEUDO_NOERROR(x, x)
#define RSYSCALL(x) PSEUDO(x, x)
#define WSYSCALL(weak, strong) \
    WEAK_ALIAS(weak, strong);  \
    PSEUDO(strong, weak)
