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
 */
	.text
	.globl	strcmp
	.type	strcmp, @function
	.p2align 4
strcmp:
	.p2align 4
.Lloop:
	ldb	r3, [r1]
	ldb	r4, [r2]
	cmp	r3, r4
	bne	.Ldiff
	test	r3, r3
	beq	.Lequal

	ldb	r3, [r1 + #1]
	ldb	r4, [r2 + #1]
	cmp	r3, r4
	bne	.Ldiff
	test	r3, r3
	beq	.Lequal

	ldb	r3, [r1 + #2]
	ldb	r4, [r2 + #2]
	cmp	r3, r4
	bne	.Ldiff
	test	r3, r3
	beq	.Lequal

	ldb	r3, [r1 + #3]
	ldb	r4, [r2 + #3]
	cmp	r3, r4
	bne	.Ldiff
	add	r1, #4
	add	r2, #4
	test	r3, r3
	bne	.Lloop

.Lequal:
	mov	r1, r0			/* equal through the NUL -> 0 */
	ret
.Ldiff:
	sub	r3, r4			/* zero-extended bytes -> signed delta */
	mov	r1, r3
	ret
	.size	strcmp, . - strcmp
