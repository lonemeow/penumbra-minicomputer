#	$NetBSD$
#
# Penumbra libkern arch configuration.
#
# Assembly string/memory functions are in common/lib/libc/arch/penumbra/string/
# and are picked up automatically via common/lib/libc/Makefile.inc.

# Byte-swap functions (from common/lib/libc/gen/).
SRCS+=	bswap16.c bswap32.c

# Suppress upstream NetBSD warnings clang flags more aggressively than gcc.
# These are in bsd.own.mk's CWARNFLAGS.clang+= block, but the userland
# lib/libkern/Makefile appends -Wall ... to CWARNFLAGS *after* that block,
# re-enabling them.  COPTS is appended at the end of the compile rule,
# after CFLAGS-derived warnings, so suppressions placed here always win.
COPTS+=	${${ACTIVE_CC} == "clang":? -Wno-unused-but-set-variable -Wno-strict-prototypes -Wno-deprecated-non-prototype :}
