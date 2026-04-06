/*	$NetBSD$	*/

/*
 * memcpy(void *dst, const void *src, size_t len) -> void *
 *   r1 = dst, r2 = src, r3 = len
 *   Returns original dst in r1
 *
 * Word-aligned fast path: when both src and dst are word-aligned,
 * copies 4 bytes at a time via ldw/stw.  ~4x faster for the
 * bulk ELF segment copies during kernel loading.
 *
 * Available registers (caller-saved): r1 (dst), r2 (src),
 *   r3 (len), r4 (saved dst), r11 (scratch).
 */
	.text
	.globl	memcpy
	.type	memcpy, @function
memcpy:
	mov	r4, r1		/* save original dst */
	test	r3, r3
	beq	.Ldone
	/* Word fast path: both src and dst must be word-aligned */
	test	r1, #3
	bne	.Lbyte
	test	r2, #3
	bne	.Lbyte

	cmp	r3, #4
	blo	.Ltail
.Lword:
	ldw	r11, [r2]
	stw	r11, [r1]
	add	r1, #4
	add	r2, #4
	sub	r3, #4
	cmp	r3, #4
	bhs	.Lword
.Ltail:
	test	r3, r3
	beq	.Ldone
.Lbyte:
	ldb	r11, [r2]
	stb	r11, [r1]
	add	r1, #1
	add	r2, #1
	sub	r3, #1
	bne	.Lbyte

.Ldone:
	mov	r1, r4		/* return original dst */
	ret
	.size	memcpy, . - memcpy
