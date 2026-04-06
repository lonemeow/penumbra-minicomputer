#	$NetBSD$
#
# Penumbra libkern arch configuration.
#
# Assembly string/memory functions are in common/lib/libc/arch/penumbra/string/
# and are picked up automatically via common/lib/libc/Makefile.inc.

# Byte-swap functions (from common/lib/libc/gen/).
SRCS+=	bswap16.c bswap32.c
