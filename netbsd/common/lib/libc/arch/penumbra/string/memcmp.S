/*	$NetBSD$	*/

/*
 * memcmp(const void *s1, const void *s2, size_t len) -> int
 *   r1 = s1, r2 = s2, r3 = len
 *   Returns <0, 0, or >0 in r1: the difference of the first unequal
 *   bytes, compared unsigned
 *
 * Whole words from 16 bytes up, whenever s1 and s2 share their
 * alignment: a byte head brings both to a word boundary first.  Shorter
 * compares keep to the byte loop's few lines -- the setup would not pay
 * for itself, and small compares are mostly cold calls.  A word that differs is
 * rescanned a byte at a time, since on a little-endian machine the
 * word's own ordering is not the bytes' ordering.  Buffers that differ
 * in alignment compare a byte at a time.
 *
 * Both loops load their two operands before advancing the pointers, so
 * neither load's value is needed for two instructions and the compare
 * never waits for it (doc/system/instruction-timing.md).
 */
	.text
	.globl	memcmp
	.type	memcmp, @function
	.p2align 4
memcmp:
	cmp	r3, #16
	blo	.Lbytes		/* too short for words to pay */
	mov	r11, r1
	xor	r11, r2
	test	r11, #3
	bne	.Lbytes		/* alignments differ: bytes only */

	/*
	 * Head: at most three bytes bring both pointers to a word boundary,
	 * and at least 13 bytes remain.
	 */
.Lhead:
	test	r1, #3
	beq	.Lwords
	ldb	r4, [r1]
	ldb	r11, [r2]
	add	r1, #1
	add	r2, #1
	sub	r3, #1
	cmp	r4, r11
	bne	.Ldiffer
	b	.Lhead

.Lwords:
	sub	r3, #4		/* r3 counts down past the next word */
	blo	.Lwtail
.Lword:
	ldw	r4, [r1]
	ldw	r11, [r2]
	add	r1, #4
	add	r2, #4
	cmp	r4, r11
	bne	.Lwdiffer
	sub	r3, #4
	bhs	.Lword
.Lwtail:
	add	r3, #4		/* 0..3 bytes left */
	b	.Lbytes

.Lwdiffer:
	sub	r1, #4		/* back to the differing word ... */
	sub	r2, #4
	lli	r3, #4		/* ... whose bytes hold the difference */

.Lbytes:
	test	r3, r3
	beq	.Lequal
.Lbyte:
	ldb	r4, [r1]
	ldb	r11, [r2]
	add	r1, #1
	add	r2, #1
	cmp	r4, r11
	bne	.Ldiffer
	sub	r3, #1
	bne	.Lbyte
.Lequal:
	mov	r1, r0		/* return 0 */
	ret
.Ldiffer:
	sub	r4, r11
	mov	r1, r4
	ret
	.size	memcmp, . - memcmp
