/*	$NetBSD$	*/

/*
 * strcpy(char *dst, const char *src) -> char *
 *   r1 = dst, r2 = src
 *   Returns original dst in r1
 *
 * Byte-at-a-time, unrolled four ways with offset addressing so the
 * pointer bumps and the loop branch amortize over four characters.
 *
 * A word-at-a-time version was tried and dropped: storing whole words
 * needs dst aligned (Penumbra has no unaligned stores), so it requires
 * src and dst to share alignment — the minority case — and its setup
 * cost regressed the common one.  The strict-alignment peers (mips,
 * m68k) keep strcpy a byte loop; x86 word-stores to an unaligned dst,
 * which Penumbra cannot.
 */
	.text
	.globl	strcpy
	.type	strcpy, @function
	.p2align 4
strcpy:
	mov	r3, r1			/* save original dst (return value) */

	.p2align 4
.Lloop:
	ldb	r4, [r2]
	stb	r4, [r1]
	test	r4, r4
	beq	.Ldone

	ldb	r4, [r2 + #1]
	stb	r4, [r1 + #1]
	test	r4, r4
	beq	.Ldone

	ldb	r4, [r2 + #2]
	stb	r4, [r1 + #2]
	test	r4, r4
	beq	.Ldone

	ldb	r4, [r2 + #3]
	stb	r4, [r1 + #3]
	add	r1, #4
	add	r2, #4
	test	r4, r4
	bne	.Lloop

.Ldone:
	mov	r1, r3			/* return original dst */
	ret
	.size	strcpy, . - strcpy
