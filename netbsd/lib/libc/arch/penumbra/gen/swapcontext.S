/*	$NetBSD$	*/

/*
 * swapcontext(oucp, ucp) for Penumbra.
 *
 * Saves the current context into oucp, then activates ucp.
 * Returns 0 on success (when oucp is later resumed), -1 on error.
 *
 * The tricky part: after _getcontext(oucp) returns, oucp's saved
 * SP and PC point inside swapcontext itself.  We must patch them
 * to point at our caller so that when oucp is later resumed via
 * setcontext(), execution continues at the call site — not here.
 *
 * Stack frame (8 bytes, matching CALLFRAME_SIZ):
 *   [SP+0]  saved oucp (R1 at entry)
 *   [SP+4]  saved LR (R13 at entry, = caller's return address)
 *
 * Register usage:
 *   R1 = oucp (first arg), then ucp for setcontext tail-call
 *   R2 = ucp (second arg), saved across _getcontext call
 *   R5 = saved oucp (callee-saved, survives _getcontext)
 *   R6 = saved ucp  (callee-saved, survives _getcontext)
 *
 * Available assym.h offsets:
 *   UC_GREGS_SP  — oucp->uc_mcontext.__gregs[_REG_SP]
 *   UC_GREGS_PC  — oucp->uc_mcontext.__gregs[_REG_PC]
 */

#include <machine/asm.h>
#include "SYS.h"
#include "assym.h"

/* TODO(human): Implement swapcontext.
 *
 * Steps:
 * 1. Allocate stack frame, save LR (R13) and callee-saved regs
 *    you'll use (R5, R6) on the stack or use the existing
 *    callee-saved convention (R5-R10 are preserved by _getcontext).
 *    Save oucp and ucp into callee-saved registers.
 *
 * 2. Call _getcontext(oucp) — R1 already has oucp.
 *    Use "bl PLT(_C_LABEL(_getcontext))".
 *    On return: R1 = return value (0 = success).
 *
 * 3. Check for error (R1 != 0 → restore frame, return error).
 *    NOTE: When oucp is later resumed by setcontext(), execution
 *    also returns here from _getcontext with R1 = 0.  At that
 *    point you must NOT fall through to step 4 — you should
 *    restore your frame and return 0 to the caller.
 *    Hint: use a flag or check whether oucp's PC was already
 *    patched to distinguish first return vs resume return.
 *    Alternatively, structure the code so that the patching in
 *    step 4 sets PC to a "return 0" landing pad, so resumed
 *    contexts skip steps 4-5 entirely.
 *
 * 4. Patch oucp: write the caller's SP and PC into oucp's
 *    saved gregs so setcontext(oucp) resumes at the caller.
 *    - oucp's SP should be our entry SP (after we pop our frame)
 *    - oucp's PC should be the saved LR (caller's return address)
 *    Use the callee-saved copy of oucp with UC_GREGS_SP/UC_GREGS_PC
 *    offsets.
 *
 * 5. Tail-call setcontext(ucp): move saved ucp to R1,
 *    then "b PLT(_C_LABEL(setcontext))".
 *    setcontext does not return on success.
 *
 * 6. Error path: restore callee-saved regs, restore LR, pop
 *    frame, return (R1 already has error code from _getcontext).
 */

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
