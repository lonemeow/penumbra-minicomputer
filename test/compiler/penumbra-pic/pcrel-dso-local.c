/* PC-relative-direct (%pcrel) materialization for non-preemptible globals.
 * static (internal) and hidden globals take the GOT-free %pcrel path, so
 * this verifies the linker resolves the PC-anchored %pcrel relocations to
 * the correct addresses and the data round-trips at runtime.  The globals
 * are volatile so every access is a real %pcrel load/store rather than a
 * constant the optimizer folds away.
 *
 * Self-checking: returns 0 only if every %pcrel-addressed datum is correct.
 */

static volatile int s_table[8] = { 2, 3, 5, 7, 11, 13, 17, 19 };
static volatile int s_accum;
static volatile char s_label[7] = "primes";
__attribute__((visibility("hidden"))) volatile int h_counter;

int main(void)
{
	int i, big = 0;

	s_accum = 0;
	for (i = 0; i < 8; i++) {
		int v = s_table[i];		/* %pcrel volatile load, across blocks */
		if (v > 10)
			big += v;		/* 11+13+17+19 = 60 */
		else
			s_accum += v;		/* %pcrel store: 2+3+5+7 = 17 */
	}
	if (big != 60)
		return 1;
	if (s_accum != 17)
		return 1;

	/* hidden global: written then read back through %pcrel. */
	h_counter = big + s_accum;	/* 77 */
	if (h_counter != 77)
		return 1;

	/* static string reached through %pcrel. */
	if (s_label[0] != 'p' || s_label[5] != 's')
		return 1;

	return 0;
}
