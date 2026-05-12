/*	$NetBSD$	*/

/*
 * Penumbra DDB machine-dependent glue.
 *
 * Provides:
 *   - ddb_regs                       MD register snapshot for the MI layer
 *   - kdb_trap()                     entry point called from trap.c on BREAK
 *   - cpu_Debugger() / Debugger()    on-demand entry (executes BREAK)
 *   - db_read_bytes / db_write_bytes safe(ish) kernel memory access
 *
 * Fault safety for db_read_bytes / db_write_bytes is provided by the MI
 * `db_recover' longjmp buffer set up by db_command_loop(); trap.c
 * checks db_recover on kernel-mode TLB/bus faults and longjmps out.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>

#include <machine/db_machdep.h>
#include <machine/frame.h>

#include <ddb/db_access.h>
#include <ddb/db_command.h>
#include <ddb/db_extern.h>
#include <ddb/db_interface.h>
#include <ddb/db_output.h>
#include <ddb/db_variables.h>

int		db_active;
db_regs_t	ddb_regs;

/*
 * Register variable table — what the MI `show registers' and
 * `set $rN = ...' commands operate on.  Each entry points directly
 * into ddb_regs; FCN_NULL means the variable is a plain `long *'.
 */
const struct db_variable db_regs[] = {
	{ "r0",  (long *)&ddb_regs.tf_regs[0],  FCN_NULL },
	{ "r1",  (long *)&ddb_regs.tf_regs[1],  FCN_NULL },
	{ "r2",  (long *)&ddb_regs.tf_regs[2],  FCN_NULL },
	{ "r3",  (long *)&ddb_regs.tf_regs[3],  FCN_NULL },
	{ "r4",  (long *)&ddb_regs.tf_regs[4],  FCN_NULL },
	{ "r5",  (long *)&ddb_regs.tf_regs[5],  FCN_NULL },
	{ "r6",  (long *)&ddb_regs.tf_regs[6],  FCN_NULL },
	{ "r7",  (long *)&ddb_regs.tf_regs[7],  FCN_NULL },
	{ "r8",  (long *)&ddb_regs.tf_regs[8],  FCN_NULL },
	{ "r9",  (long *)&ddb_regs.tf_regs[9],  FCN_NULL },
	{ "r10", (long *)&ddb_regs.tf_regs[10], FCN_NULL },
	{ "r11", (long *)&ddb_regs.tf_regs[11], FCN_NULL },
	{ "tp",  (long *)&ddb_regs.tf_regs[12], FCN_NULL },
	{ "lr",  (long *)&ddb_regs.tf_regs[13], FCN_NULL },
	{ "sp",  (long *)&ddb_regs.tf_regs[14], FCN_NULL },
	{ "r15", (long *)&ddb_regs.tf_regs[15], FCN_NULL },
	{ "sr",  (long *)&ddb_regs.tf_sr,       FCN_NULL },
	{ "pc",  (long *)&ddb_regs.tf_epc,      FCN_NULL },
};
const struct db_variable * const db_eregs = db_regs + __arraycount(db_regs);

int
kdb_trap(int type, struct trapframe *tf)
{

	switch (type) {
	case 6: /* EXC_BREAK / VEC_BREAK */
	case -1: /* synthetic entry from panic / cn_check_magic */
		break;
	default:
		if (db_recover != NULL)
			longjmp(db_recover);
		return 0;
	}

	/*
	 * Snapshot the trapframe for MI inspection commands.  On resume
	 * we copy it back so `db> set $rN = ...` and `db> set $epc = ...`
	 * take effect.
	 */
	ddb_regs = *tf;
	db_trap(type, 0);
	*tf = ddb_regs;

	return 1;
}

void
cpu_Debugger(void)
{

	__asm volatile ("break");
}

/*
 * MI memory access primitives.
 *
 * No special fault recovery is needed at this layer: the MI command
 * loop wraps each command in setjmp(db_recover), and trap.c will
 * longjmp() through it on a kernel-mode TLB or bus fault.  So a bad
 * VA passed to db_read_bytes simply aborts the in-progress command
 * and returns to the `db>' prompt.
 */
void
db_read_bytes(vaddr_t addr, size_t size, char *data)
{
	const char *src;

	src = (const char *)addr;
	while (size-- > 0)
		*data++ = *src++;
}

void
db_write_bytes(vaddr_t addr, size_t size, const char *data)
{
	char *dst;

	dst = (char *)addr;
	while (size-- > 0)
		*dst++ = *data++;
}
