/*	$NetBSD$	*/

/*
 * strlen(const char *str) -> size_t
 *   r1 = str pointer
 *   Returns length in r1
 *
 * Word-at-a-time: a byte prologue advances the pointer to a 4-byte
 * boundary, then an aligned word loop tests four bytes per iteration for
 * a NUL with the classic (w - 0x01010101) & ~w & 0x80808080 detector.
 * An aligned word load never straddles a page, and we advance to the
 * next word only after proving the current one holds no terminator, so
 * the loop never reads past the string's mapped page.
 *
 * The detector needs both magic constants live at once, one more value
 * than the r1-r4/r11 scratch pool holds, so the word loop borrows
 * callee-saved r5 for 0x80808080.  That save is deferred until after the
 * byte prologue: a string whose NUL lands in the unaligned head returns
 * without ever touching the stack.  Penumbra has no count-leading-zeros,
 * so the terminator's lane is found by shifting its 0x80 to the sign bit
 * (cf. sparc64, which likewise scans the detector mask byte-wise).
 */
	.text
	.globl	strlen
	.type	strlen, @function
	.p2align 4
strlen:
	mov	r2, r1			/* r2 = scan pointer (r1 keeps start) */

	/* Byte prologue: walk to a word boundary, no frame, no constants. */
.Lhead:
	test	r2, #3
	beq	.Lsetup
	ldb	r3, [r2]
	test	r3, r3
	beq	.Lret_nf		/* NUL in the head -> frameless return */
	add	r2, #1
	b	.Lhead

.Lsetup:
	/* Aligned and not yet terminated: commit to the word loop. */
	sub	sp, #4
	stw	r5, [sp]
	li	r4, #0x01010101
	li	r5, #0x80808080

	.p2align 4
.Lword:
	ldw	r3, [r2]		/* aligned 4-byte chunk */
	mov	r11, r3
	sub	r11, r4			/* w - 0x01010101 */
	not	r3, r3			/* ~w */
	and	r11, r3			/* (w - C1) & ~w */
	and	r11, r5			/* & 0x80808080; Z set iff no NUL byte */
	bne	.Lhaszero
	add	r2, #4
	b	.Lword

	/*
	 * r11 = mask with 0x80 in each NUL byte's lane.  Find the lowest-
	 * address flagged byte by shifting its 0x80 up to the sign bit.
	 */
.Lhaszero:
	mov	r3, r11
	shl	r3, #24			/* low byte's flag -> sign bit */
	bmi	.Lret
	add	r2, #1
	shr	r11, #8			/* bring the next byte's flag down */
	b	.Lhaszero

.Lret:
	ldw	r5, [sp]		/* restore callee-saved, then fall through */
	add	sp, #4
.Lret_nf:
	sub	r2, r1			/* length = scan - start */
	mov	r1, r2
	ret
	.size	strlen, . - strlen
