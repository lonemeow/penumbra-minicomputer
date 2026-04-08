/*	$NetBSD$	*/

/*
 * Mark end of .eh_frame and .jcr sections.
 */

#include <machine/asm.h>

RCSID("$NetBSD$")

	.section	.eh_frame, "a", @progbits
	.p2align 2
	.global		__EH_FRAME_END__
	.hidden		__EH_FRAME_END__
__EH_FRAME_END__:
	.space	4

	.section	.jcr, "aw", @progbits
	.p2align 2
	.global		__JCR_END__
	.hidden		__JCR_END__
__JCR_END__:
	.space	4
