#	$NetBSD$
#
#	etc.penumbra/Makefile.inc -- penumbra-specific etc Makefile targets
#

KERNEL_SETS=		MINIMAL

snap_md_post: check_DESTDIR check_RELEASEDIR snap_post
