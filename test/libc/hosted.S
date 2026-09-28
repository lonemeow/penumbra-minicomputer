/*
 * hosted_write(fd, buf, len): the ISS's hosted-mode write syscall
 * (R11 = 4), so the string test prints without a libc.
 */
	.text
	.globl	hosted_write
	.type	hosted_write, @function
hosted_write:
	lli	r11, #4
	syscall
	ret
	.size	hosted_write, . - hosted_write
