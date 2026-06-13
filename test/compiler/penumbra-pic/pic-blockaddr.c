/* PIC block-address materialization (computed goto, MOV+ADDi anchor).
 * &&label taken in PIC mode forms a PC-relative address through the
 * same anchor path as the jump-table base.  A threaded dispatch loop
 * walks a program of label addresses, updating a GOT-loaded global.
 *
 * Self-checking: returns 0 only if the threaded walk lands correctly.
 */

int result;

int main(void)
{
	/* &&labels are local; dispatch[] holds them in program order. */
	static const void *prog[] = { &&add3, &&dbl, &&add3, &&dbl, &&stop };
	int pc = 0;

	result = 1;
	goto *prog[pc];

add3:
	result += 3;
	pc++;
	goto *prog[pc];

dbl:
	result *= 2;
	pc++;
	goto *prog[pc];

stop:
	/* ((1 + 3) * 2 + 3) * 2 = 22 */
	return result == 22 ? 0 : 1;
}
