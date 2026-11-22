/*	$NetBSD$	*/

/*
 * swapcontext(oucp, ucp) for Penumbra.
 *
 * Saves the current context into oucp, then activates ucp.  Returns -1,
 * with errno set, if oucp cannot be saved or ucp cannot be activated;
 * resuming oucp later returns 0 to the caller.
 *
 * _getcontext records the context of its own call, which is inside this
 * function, and already stores the 0 a resumed call returns.  Before ucp
 * is activated, the saved SP and PC are rewritten to the caller's, so
 * resuming oucp returns from swapcontext instead of re-entering it.
 * Nothing callee-saved is touched beforehand, so the copies _getcontext
 * keeps are the caller's.
 *
 * The 12-byte frame holds LR, oucp and ucp across the _getcontext call.
 */

#include <machine/asm.h>
#include "SYS.h"
#include "assym.h"

ENTRY(swapcontext)

    sub sp, 12
    stw lr, [sp + 0]
    stw r1, [sp + 4]
    stw r2, [sp + 8]

    bl PLT(_C_LABEL(_getcontext))

    ldw lr, [sp + 0]
    ldw r2, [sp + 4]
    ldw r3, [sp + 8]
    add sp, 12

    cmp r1, r0
    bne .Lexit

    stw sp, [r2 + UC_GREGS_SP]
    stw lr, [r2 + UC_GREGS_PC]

    mov r1, r3
    b PLT(_C_LABEL(setcontext))

.Lexit:
    ret

END(swapcontext)
