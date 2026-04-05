/*	$NetBSD$	*/

/*
 * memset(void *dst, int c, size_t len) -> void *
 *   r1 = dst, r2 = fill byte, r3 = len
 *   Returns original dst in r1
 */
	.text
	.globl	memset
	.type	memset, @function
memset:
	mov	r4, r1		/* save original dst */
	test	r3, r3
	beq	.Ldone
.Lloop:
	stb	r2, [r1]
	add	r1, #1
	sub	r3, #1
	bne	.Lloop
.Ldone:
	mov	r1, r4		/* return original dst */
	ret
	.size	memset, . - memset
