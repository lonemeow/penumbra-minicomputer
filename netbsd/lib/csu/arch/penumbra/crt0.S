/*	$NetBSD$	*/

/*
 * C runtime startup for Penumbra.
 *
 * The kernel's setregs() sets up:
 *   R1 = 0 (cleanup, unused for static binaries)
 *   R2 = ps_strings pointer
 *   R14 = initial stack pointer
 *   PC = entry point (_start)
 *
 * ___start(cleanup, ps_strings) is in crt0-common.c.
 * Our calling convention already has R1=arg1, R2=arg2,
 * so we just jump straight there.
 */

#include <machine/asm.h>

RCSID("$NetBSD$")

ENTRY(__start)
	b	___start
END(__start)

STRONG_ALIAS(_start,__start)
