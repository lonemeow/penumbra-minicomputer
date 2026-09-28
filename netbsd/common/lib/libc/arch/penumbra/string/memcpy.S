/*	$NetBSD$	*/

/*
 * memcpy(void *dst, const void *src, size_t len) -> void *
 *   r1 = dst, r2 = src, r3 = len
 *   Returns original dst in r1
 *
 * Whole words whenever src and dst share their alignment: a byte head
 * brings both to a word boundary first.  Pointers that differ in
 * alignment copy a byte at a time.
 *
 * The loops are ordered for the pipelined cores
 * (doc/system/instruction-timing.md): a load's value is first used two
 * instructions after it, where its reader no longer waits, and a store
 * sits directly before each loop's closing branch, where its stall
 * hides the taken branch's fetch bubble.  The word loop moves two words
 * per iteration through the one scratch register.
 *
 * Available registers (caller-saved): r1 (dst), r2 (src),
 *   r3 (len), r4 (saved dst), r11 (scratch).
 */
	.text
	.globl	memcpy
	.type	memcpy, @function
	/*
	 * Only the entry is aligned to the 16-byte L1 I-cache line.  With a
	 * 2-way L1 a loop straddling two lines pays the second fetch on its
	 * first pass only, while padding NOPs ahead of a loop would lengthen
	 * every call's path through more lines -- cold calls most, and the
	 * kernel's small ones are mostly cold.
	 */
	.p2align 4
memcpy:
	mov	r4, r1		/* save original dst */
	cmp	r3, #4
	blo	.Lbytes		/* too short for a word */
	mov	r11, r1
	xor	r11, r2
	test	r11, #3
	bne	.Lbytes		/* alignments differ: bytes only */

	/*
	 * Head: at most three bytes bring both pointers to a word boundary.
	 * len >= 4, so at least one byte remains.
	 */
.Lhead:
	test	r1, #3
	beq	.Lwords
	ldb	r11, [r2]
	add	r2, #1
	add	r1, #1
	sub	r3, #1
	stb	r11, [r1 + #-1]
	b	.Lhead

.Lwords:
	sub	r3, #8		/* r3 counts down past the next two words */
	blo	.Lword
.Lword2:
	ldw	r11, [r2]
	add	r2, #8
	stw	r11, [r1]
	ldw	r11, [r2 + #-4]
	add	r1, #8
	sub	r3, #8
	stw	r11, [r1 + #-4]
	bhs	.Lword2
.Lword:
	add	r3, #8		/* 0..7 bytes left */
	test	r3, #4
	beq	.Ltail
	ldw	r11, [r2]
	add	r2, #4
	add	r1, #4
	stw	r11, [r1 + #-4]
.Ltail:
	and	r3, #3
.Lbytes:
	test	r3, r3
	beq	.Ldone
.Lbyte:
	ldb	r11, [r2]
	add	r2, #1
	add	r1, #1
	sub	r3, #1
	stb	r11, [r1 + #-1]
	bne	.Lbyte

.Ldone:
	mov	r1, r4		/* return original dst */
	ret
	.size	memcpy, . - memcpy
