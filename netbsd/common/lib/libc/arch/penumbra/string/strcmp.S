/*	$NetBSD$	*/

/*
 * strcmp(const char *s1, const char *s2) -> int
 *   r1 = s1, r2 = s2
 *   Returns <0, 0, or >0 in r1
 *
 * Byte-at-a-time, unrolled four ways with offset addressing so the
 * pointer bumps and the loop branch amortize over four characters.
 *
 * A word-at-a-time version was tried and dropped: it pays off only when
 * s1 and s2 share alignment (Penumbra has no unaligned loads), which is
 * the minority case, and the setup cost regressed the common one.  The
 * strict-alignment peers (mips, m68k, sparc32) all keep strcmp a byte
 * loop for the same reason; only x86, with cheap unaligned access, word-
 * optimizes it.
 *
 * Each step tests s1's byte for the terminator before comparing it with
 * s2's, so s2's byte is not needed until two instructions after its load
 * and the compare never waits for it (doc/system/instruction-timing.md).
 * A terminator in s1 goes straight to the difference, which is zero
 * exactly when s2 ends there too.
 */
	.text
	.globl	strcmp
	.type	strcmp, @function
	.p2align 4
strcmp:
.Lloop:
	ldb	r3, [r1]
	ldb	r4, [r2]
	test	r3, r3
	beq	.Ldiff
	cmp	r3, r4
	bne	.Ldiff

	ldb	r3, [r1 + #1]
	ldb	r4, [r2 + #1]
	test	r3, r3
	beq	.Ldiff
	cmp	r3, r4
	bne	.Ldiff

	ldb	r3, [r1 + #2]
	ldb	r4, [r2 + #2]
	test	r3, r3
	beq	.Ldiff
	cmp	r3, r4
	bne	.Ldiff

	ldb	r3, [r1 + #3]
	ldb	r4, [r2 + #3]
	add	r1, #4
	add	r2, #4
	test	r3, r3
	beq	.Ldiff
	cmp	r3, r4
	beq	.Lloop

.Ldiff:
	sub	r3, r4			/* zero-extended bytes -> signed delta */
	mov	r1, r3
	ret
	.size	strcmp, . - strcmp
