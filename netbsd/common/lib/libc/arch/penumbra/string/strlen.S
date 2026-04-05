/*	$NetBSD$	*/

/*
 * strlen(const char *str) -> size_t
 *   r1 = str pointer
 *   Returns length in r1
 */
	.text
	.globl	strlen
	.type	strlen, @function
strlen:
	mov	r2, r1		/* r2 = scan pointer */
.Lloop:
	ldb	r3, [r2]	/* load byte */
	add	r2, #1
	test	r3, r3		/* NUL? */
	bne	.Lloop
	sub	r2, r1		/* r2 = end - start */
	sub	r2, #1		/* adjust for post-increment past NUL */
	mov	r1, r2
	ret
	.size	strlen, . - strlen
