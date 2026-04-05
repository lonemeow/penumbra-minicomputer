/*	$NetBSD$	*/

/*
 * memcmp(const void *s1, const void *s2, size_t len) -> int
 *   r1 = s1, r2 = s2, r3 = len
 *   Returns <0, 0, or >0 in r1
 */
	.text
	.globl	memcmp
	.type	memcmp, @function
memcmp:
	test	r3, r3
	beq	.Lequal
.Lloop:
	ldb	r4, [r1]
	ldb	r11, [r2]
	cmp	r4, r11
	bne	.Ldiffer
	add	r1, #1
	add	r2, #1
	sub	r3, #1
	bne	.Lloop
.Lequal:
	mov	r1, r0		/* return 0 */
	ret
.Ldiffer:
	sub	r4, r11
	mov	r1, r4
	ret
	.size	memcmp, . - memcmp
