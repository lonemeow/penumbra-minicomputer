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
 *
 * The copy is software-pipelined across the unrolled steps: a character
 * is loaded once the one before it has tested non-zero, so the loop
 * never reads past the terminator, and stored a step later, so the store
 * never waits for its load (doc/system/instruction-timing.md).  A zero
 * falls into the add chain that puts the terminator at its offset.
 */
	.text
	.globl	strcpy
	.type	strcpy, @function
	.p2align 4
strcpy:
	mov	r3, r1			/* save original dst (return value) */
	ldb	r4, [r2]		/* the first character */

.Lloop:					/* r4 = the character for [r1] */
	test	r4, r4
	beq	.Lnul0
	ldb	r11, [r2 + #1]
	stb	r4, [r1]
	test	r11, r11
	beq	.Lnul1
	ldb	r4, [r2 + #2]
	stb	r11, [r1 + #1]
	test	r4, r4
	beq	.Lnul2
	ldb	r11, [r2 + #3]
	stb	r4, [r1 + #2]
	test	r11, r11
	beq	.Lnul3
	ldb	r4, [r2 + #4]
	add	r1, #4
	add	r2, #4
	stb	r11, [r1 + #-1]
	b	.Lloop

.Lnul3:	add	r1, #1
.Lnul2:	add	r1, #1
.Lnul1:	add	r1, #1
.Lnul0:	stb	r0, [r1]		/* the terminator */
	mov	r1, r3			/* return original dst */
	ret
	.size	strcpy, . - strcpy
