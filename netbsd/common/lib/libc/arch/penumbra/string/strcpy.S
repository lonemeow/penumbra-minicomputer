/*	$NetBSD$	*/

/*
 * strcpy(char *dst, const char *src) -> char *
 *   r1 = dst, r2 = src
 *   Returns original dst in r1
 */
	.text
	.globl	strcpy
	.type	strcpy, @function
strcpy:
	mov	r3, r1		/* save original dst */
.Lloop:
	ldb	r4, [r2]
	stb	r4, [r1]
	add	r1, #1
	add	r2, #1
	test	r4, r4
	bne	.Lloop
	mov	r1, r3		/* return original dst */
	ret
	.size	strcpy, . - strcpy
