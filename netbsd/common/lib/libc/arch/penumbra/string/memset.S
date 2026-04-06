/*	$NetBSD$	*/

/*
 * memset(void *dst, int c, size_t len) -> void *
 *   r1 = dst, r2 = fill byte, r3 = len
 *   Returns original dst in r1
 *
 * Word-aligned fast path: aligns dst, replicates the fill byte
 * to all 4 lanes, then stores words at a time.  ~4x faster for
 * bulk operations like BSS zeroing.
 */
	.text
	.globl	memset
	.type	memset, @function
memset:
	mov	r4, r1		/* save original dst */
	test	r3, r3
	beq	.Ldone

	/* Handle leading unaligned bytes */
	test	r1, #3
	beq	.Laligned
.Lhead:
	stb	r2, [r1]
	add	r1, #1
	sub	r3, #1
	beq	.Ldone
	test	r1, #3
	bne	.Lhead

.Laligned:
	cmp	r3, #4
	blo	.Ltail

	/* Replicate fill byte to all 4 lanes of r2.
	 * stb in the tail loop still works because
	 * the low byte is unchanged. */
	and	r2, #0xFF
	mov	r11, r2
	shl	r11, #8
	or	r2, r11		/* r2 = 0x0000CCCC */
	mov	r11, r2
	shl	r11, #16
	or	r2, r11		/* r2 = 0xCCCCCCCC */

.Lword:
	stw	r2, [r1]
	add	r1, #4
	sub	r3, #4
	cmp	r3, #4
	bhs	.Lword

.Ltail:
	test	r3, r3
	beq	.Ldone
.Ltail_loop:
	stb	r2, [r1]
	add	r1, #1
	sub	r3, #1
	bne	.Ltail_loop

.Ldone:
	mov	r1, r4		/* return original dst */
	ret
	.size	memset, . - memset
