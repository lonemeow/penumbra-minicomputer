/*	$NetBSD$	*/

/*
 * Stub disassembler for Penumbra DDB.
 *
 * The minimal-useful DDB scope does not include real instruction
 * decoding; we print raw 32-bit words.  The MI `x/i' command still
 * works (no crash), it just shows hex rather than mnemonics.
 *
 * A real decoder could later be derived from the LLVM backend's
 * Disassembler/ directory (see llvm/llvm/lib/Target/Penumbra/).
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>

#include <machine/db_machdep.h>

#include <ddb/db_access.h>
#include <ddb/db_output.h>
#include <ddb/db_interface.h>

db_addr_t
db_disasm(db_addr_t loc, bool altfmt)
{
	uint32_t insn;

	db_read_bytes(loc, sizeof(insn), (char *)&insn);
	db_printf(".long\t0x%08x\n", insn);
	return loc + 4;
}
