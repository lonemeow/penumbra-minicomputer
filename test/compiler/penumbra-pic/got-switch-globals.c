/* PIC GOT anchor regression — a faithful miniature of libc's
 * dl_iterate_phdr_setup, the code that broke when PIC GOT offsets were
 * anchored by a fixed -8/-4 instead of their own instruction.
 *
 * parse() loops over key/value pairs and, on each runtime-variable key,
 * stores the value through one of several distinct file-scope globals.
 * Every case ends in "store; continue", so they share a tail at the
 * loop latch.  The optimizer clones the GOT-address materialization
 * into each case and branch folding merges the identical tails; if the
 * PC anchor of a case's LLI/LUI pair is not its own, the merged form
 * computes a neighboring GOT slot and the value lands in the wrong
 * global — exactly the auxv corruption that killed init.
 *
 * parse() is noinline and driven by runtime data so the switch and its
 * tail-merge survive optimization (an inlined, constant-folded version
 * would not exercise the bug).  Self-checking: returns 0 only if every
 * value reaches its intended global.
 */

int g_base, g_phdr, g_phnum, g_execname, g_pagesz, g_flags;

__attribute__((noinline))
void parse(const int *kv)
{
	for (; kv[0] != 0; kv += 2) {
		switch (kv[0]) {
		case 7:    g_base     = kv[1]; break;
		case 3:    g_phdr     = kv[1]; break;
		case 5:    g_phnum    = kv[1]; break;
		case 2014: g_execname = kv[1]; break;
		case 6:    g_pagesz   = kv[1]; break;
		case 8:    g_flags    = kv[1]; break;
		}
	}
}

int main(void)
{
	/* volatile so the optimizer cannot fold the keys into parse() and
	 * specialize away the switch. */
	static volatile int src[] = {
		7, 1000, 3, 2000, 5, 3000, 2014, 4000, 6, 5000, 8, 6000, 0, 0
	};
	int kv[14];
	int i;

	for (i = 0; i < 14; i++)
		kv[i] = src[i];

	parse(kv);

	if (g_base     != 1000) return 1;
	if (g_phdr     != 2000) return 1;
	if (g_phnum    != 3000) return 1;
	if (g_execname != 4000) return 1;
	if (g_pagesz   != 5000) return 1;
	if (g_flags    != 6000) return 1;

	return 0;
}
