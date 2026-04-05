/*	$NetBSD$	*/

/*
 * memcpy(void *dst, const void *src, size_t len) -> void *
 *   r1 = dst, r2 = src, r3 = len
 *   Returns original dst in r1
 *
 * Byte-at-a-time for now.  TODO: word-aligned fast path.
 */
	.text
	.globl	memcpy
	.type	memcpy, @function
memcpy:
	mov	r4, r1		/* save original dst */
	test	r3, r3
	beq	.Ldone
.Lloop:
	ldb	r11, [r2]
	stb	r11, [r1]
	add	r1, #1
	add	r2, #1
	sub	r3, #1
	bne	.Lloop
.Ldone:
	mov	r1, r4		/* return original dst */
	ret
	.size	memcpy, . - memcpy
