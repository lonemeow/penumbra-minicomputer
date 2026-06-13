/* PIC GOT materialization for global array and scalar access across
 * several basic blocks.  Exercises repeated GOT-address loads of the
 * same and different symbols where the optimizer may hoist, clone, or
 * merge the anchor sequences.
 *
 * Self-checking: returns 0 only if the GOT-loaded data is correct.
 */

int table[8] = { 2, 3, 5, 7, 11, 13, 17, 19 };
int accum;
const char *label = "primes";

static int sum_if(int threshold)
{
	int i, s = 0;
	for (i = 0; i < 8; i++) {
		if (table[i] > threshold)
			s += table[i];
		else
			accum += table[i];
	}
	return s;
}

int main(void)
{
	int big;

	accum = 0;
	big = sum_if(10);		/* 11+13+17+19 = 60 */
	if (big != 60)
		return 1;
	if (accum != 2 + 3 + 5 + 7)	/* 17 */
		return 1;

	/* String literal reached through the GOT. */
	if (label[0] != 'p' || label[5] != 's')
		return 1;

	return 0;
}
