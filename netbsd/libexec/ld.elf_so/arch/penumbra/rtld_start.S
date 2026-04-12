/*	$NetBSD$	*/

/*
 * Penumbra dynamic linker entry point and PLT resolver stub.
 *
 * Entry state (from kernel exec of dynamically-linked binary):
 *   SP = user stack (argc, argv, envp, auxv)
 *   R1 = 0 (cleanup, filled by rtld)
 *   R2 = ps_strings
 *   PC = _rtld_start
 *
 * Callee-saved: R5-R10.  Scratch: R1-R4, R11.  R13=LR, R14=SP.
 */

#include <machine/asm.h>

	.globl _C_LABEL(_rtld_relocate_nonplt_self)
	.globl _C_LABEL(_rtld)

/*
 * Bias computation:
 *
 * All pool entries are offsets from .Lpc (the MOV PC anchor).
 * Pool is placed AFTER .Lpc so LDW offsets are positive.
 * Pool values are cross-section differences resolved by the
 * static linker (R_PENUMBRA_PC32) — no dynamic relocs.
 *
 *   runtime _DYNAMIC = runtime .Lpc + pool[0]
 *   runtime .Ldynamic_linkaddr = runtime .Lpc + pool[1]
 *   linktime _DYNAMIC = *(.Ldynamic_linkaddr)  (pre-relocation)
 *   bias = runtime _DYNAMIC - linktime _DYNAMIC
 */
ENTRY(_rtld_start)
	mov	r6, r2			/* save ps_strings */

.Lpc:
	mov	r2, r15			/* r2 = runtime .Lpc */

	/* runtime _DYNAMIC */
	ldw	r3, [r2 + (.Lpool_dyn - .Lpc)]
	mov	r1, r2
	add	r1, r3			/* r1 = runtime _DYNAMIC */

	/* linktime _DYNAMIC from .data word */
	ldw	r3, [r2 + (.Lpool_linkaddr - .Lpc)]
	mov	r4, r2
	add	r4, r3			/* r4 = runtime &.Ldynamic_linkaddr */
	ldw	r3, [r4]		/* r3 = linktime _DYNAMIC (pre-reloc) */

	/* bias */
	mov	r5, r1
	sub	r5, r3			/* r5 = bias = runtime - linktime */

	mov	r2, r5			/* arg2 = relocbase */
	bl	_rtld_relocate_nonplt_self

	sub	sp, 8
	mov	r1, sp			/* arg1 = &storearea */
	mov	r2, r5			/* arg2 = relocbase */
	bl	_rtld			/* returns entry point in r1 */

	mov	r11, r1			/* entry point */
	ldw	r1, [sp]		/* cleanup */
	ldw	r2, [sp + 4]		/* obj_main */
	mov	r3, r6			/* ps_strings */
	add	sp, 8
	jmp	r11

	/* Literal pool: offsets from .Lpc, resolved by static linker */
	.align	2
.Lpool_dyn:
	.long	_DYNAMIC - .Lpc
.Lpool_linkaddr:
	.long	.Ldynamic_linkaddr - .Lpc
END(_rtld_start)

/*
 * _rtld_bind_start: lazy PLT resolver entry (placeholder).
 * Eager binding only for now.
 */

#define BIND_FRAME_SIZ	(8 * SZREG)
#define BIND_FRAME_LR	(7 * SZREG)
#define BIND_FRAME_R4	(6 * SZREG)
#define BIND_FRAME_R3	(5 * SZREG)
#define BIND_FRAME_R2	(4 * SZREG)
#define BIND_FRAME_R1	(3 * SZREG)

ENTRY_NP(_rtld_bind_start)
	sub	sp, #BIND_FRAME_SIZ
	stw	r1,  [sp + BIND_FRAME_R1]
	stw	r2,  [sp + BIND_FRAME_R2]
	stw	r3,  [sp + BIND_FRAME_R3]
	stw	r4,  [sp + BIND_FRAME_R4]
	stw	r13, [sp + BIND_FRAME_LR]

	bl	_C_LABEL(_rtld_bind)
	mov	r11, r1

	ldw	r1,  [sp + BIND_FRAME_R1]
	ldw	r2,  [sp + BIND_FRAME_R2]
	ldw	r3,  [sp + BIND_FRAME_R3]
	ldw	r4,  [sp + BIND_FRAME_R4]
	ldw	r13, [sp + BIND_FRAME_LR]
	add	sp, #BIND_FRAME_SIZ

	jmp	r11
END(_rtld_bind_start)

	/* Link-time _DYNAMIC in writable .data — gets R_RELATIVE.
	 * Before self-relocation, holds original link-time value. */
	.data
	.align	4
.Ldynamic_linkaddr:
	.long	_DYNAMIC
