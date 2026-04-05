/*	$NetBSD$	*/

/*
 * strcmp(const char *s1, const char *s2) -> int
 *   r1 = s1, r2 = s2
 *   Returns <0, 0, or >0 in r1
 */
	.text
	.globl	strcmp
	.type	strcmp, @function
strcmp:
.Lloop:
	ldb	r3, [r1]
	ldb	r4, [r2]
	cmp	r3, r4
	bne	.Ldiffer
	test	r3, r3		/* both equal — NUL? */
	beq	.Lequal
	add	r1, #1
	add	r2, #1
	b	.Lloop
.Ldiffer:
	sub	r3, r4
	mov	r1, r3
	ret
.Lequal:
	mov	r1, r0		/* return 0 */
	ret
	.size	strcmp, . - strcmp
