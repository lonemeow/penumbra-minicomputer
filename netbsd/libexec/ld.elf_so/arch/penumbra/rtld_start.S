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
 * Protocol:
 *   1. Compute load bias (same technique as bootloader crt0.S)
 *   2. Self-relocate via _rtld_relocate_nonplt_self(dynp, relocbase)
 *   3. Call _rtld(sp, relocbase) -> returns entry point
 *   4. Jump to entry with (cleanup, obj_main, ps_strings) in R1-R3
 *
 * Callee-saved: R5-R10.  Scratch: R1-R4, R11.  R13=LR, R14=SP.
 */

#include <machine/asm.h>

	.globl _C_LABEL(_rtld_relocate_nonplt_self)
	.globl _C_LABEL(_rtld)

/*
 * _rtld_start: dynamic linker entry point.
 *
 * The kernel sets up the stack and jumps here.  We must
 * self-relocate, then call _rtld() to process all shared
 * objects, and finally jump to the program's real entry point.
 *
 * _rtld() writes two pointers to the stack area we pass:
 *   sp[0] = cleanup function pointer
 *   sp[1] = obj_main pointer
 *
 * The program's ___start(cleanup, obj_main, ps_strings) expects
 * these in R1, R2, R3.
 */
ENTRY(_rtld_start)
	mov r6, r2 /* Save ps_strings to a callee-saved register */

	mov r2, pc
	add r2, 8
	add r2, %pcrel(_DYNAMIC) /* r2 = Runtime address of _DYNAMIC (PC-relative) */

    mov  r3, pc
    add  r3, 8
    add  r3, %pcrel(.Ldynamic_linkaddr)
    ldw  r3, [r3] /* r3 = link-time _DYNAMIC (read from .data, pre-relocation) */

    mov  r5, r2
    sub  r5, r3 /* r5 = Relocbase */

	mov r1, r2
	mov r2, r5
	bl  _rtld_relocate_nonplt_self /* _rtld_relocate_nonplt_self(dynamic_pcrel, relocbase) */

	sub sp, 8 /* Allocate 2 words on stack for cleanup, obj_main */
	mov r1, sp
	mov r2, r5
	bl  _rtld /* _rtld(storearea, relocbase) */

	mov r11, r1       /* entry point */
	ldw r1, [sp]      /* cleanup */
	ldw r2, [sp + 4]  /* obj_main */
	mov r3, r6        /* ps_strings */
	add sp, 8
	jmp r11
END(_rtld_start)

/*
 * _rtld_bind_start: lazy PLT resolver entry.
 *
 * Called from PLT header when a lazy GOT.PLT slot is invoked.
 * GOT.PLT[0] = &_rtld_bind_start (set by _rtld_setup_pltgot)
 * GOT.PLT[1] = obj pointer
 *
 * For now we use eager binding only (_rtld_relocate_plt_lazy
 * is a no-op), so this stub should never be reached.  Include
 * it as a placeholder for future lazy binding support.
 *
 * If reached, save all argument registers, call _rtld_bind,
 * restore arguments, and jump to the resolved function.
 */

#define BIND_FRAME_SIZ	(8 * SZREG)
#define BIND_FRAME_LR	(7 * SZREG)
#define BIND_FRAME_R4	(6 * SZREG)
#define BIND_FRAME_R3	(5 * SZREG)
#define BIND_FRAME_R2	(4 * SZREG)
#define BIND_FRAME_R1	(3 * SZREG)
#define BIND_FRAME_OBJ	(1 * SZREG)
#define BIND_FRAME_OFF	(0 * SZREG)

ENTRY_NP(_rtld_bind_start)
	/* Save argument registers and LR */
	sub	sp, #BIND_FRAME_SIZ
	stw	r1,  [sp + BIND_FRAME_R1]
	stw	r2,  [sp + BIND_FRAME_R2]
	stw	r3,  [sp + BIND_FRAME_R3]
	stw	r4,  [sp + BIND_FRAME_R4]
	stw	r13, [sp + BIND_FRAME_LR]

	/* _rtld_bind(obj, reloff) */
	/* obj and reloff must be passed by PLT header — TBD */
	bl	_C_LABEL(_rtld_bind)
	mov	r11, r1			/* save resolved address */

	/* Restore argument registers and LR */
	ldw	r1,  [sp + BIND_FRAME_R1]
	ldw	r2,  [sp + BIND_FRAME_R2]
	ldw	r3,  [sp + BIND_FRAME_R3]
	ldw	r4,  [sp + BIND_FRAME_R4]
	ldw	r13, [sp + BIND_FRAME_LR]
	add	sp, #BIND_FRAME_SIZ

	/* Jump to resolved function */
	jmp	r11
END(_rtld_bind_start)

    .data
    .align 4
.Ldynamic_linkaddr:
    .long _DYNAMIC
