/* PIC jump-table dispatch combined with GOT-loaded globals.  A dense
 * switch on a runtime selector lowers to a jump table whose base is
 * materialized PC-relative (MOV+ADDi anchor); each case then updates a
 * distinct file-scope global through its own GOT sequence.  Both anchor
 * mechanisms — the jump-table base and each case's GOT offset — must
 * resolve against their own reference point.  Distinct globals (rather
 * than one array) force a separate GOT anchor per case, so a regression
 * that mispairs an anchor lands a value in the wrong global.
 *
 * dispatch() is noinline and driven by runtime data so the jump table
 * and its per-case GOT sequences survive optimization.  Self-checking:
 * returns 0 only if every case updated its own global.
 */

int c0, c1, c2, c3, c4, c5, c6, c7;

__attribute__((noinline))
void dispatch(const int *op, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		switch (op[i]) {
		case 0: c0 += 1;      break;
		case 1: c1 += 10;     break;
		case 2: c2 += 100;    break;
		case 3: c3 += 1000;   break;
		case 4: c4 += 10000;  break;
		case 5: c5 += 100000; break;
		case 6: c6 += 7;      break;
		case 7: c7 += 9;      break;
		}
	}
}

int main(void)
{
	static volatile int prog[] = {
		0, 2, 2, 5, 1, 3, 3, 3, 4, 0, 6, 7, 7, 6, 6
	};
	int ops[15];
	int i;

	for (i = 0; i < 15; i++)
		ops[i] = prog[i];

	dispatch(ops, 15);

	if (c0 != 2)      return 1;	/* op 0 x2  */
	if (c1 != 10)     return 1;	/* op 1 x1  */
	if (c2 != 200)    return 1;	/* op 2 x2  */
	if (c3 != 3000)   return 1;	/* op 3 x3  */
	if (c4 != 10000)  return 1;	/* op 4 x1  */
	if (c5 != 100000) return 1;	/* op 5 x1  */
	if (c6 != 21)     return 1;	/* op 6 x3 * 7  */
	if (c7 != 18)     return 1;	/* op 7 x2 * 9  */

	return 0;
}
