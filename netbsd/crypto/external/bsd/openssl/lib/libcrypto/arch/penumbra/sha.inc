#	$NetBSD$
#
# Penumbra: use generic C implementations of SHA.
# The default sha.inc uses libc-sha*.c wrappers that reference
# SHA functions from libc, but the version script requires them
# to be defined in libcrypto itself.

.PATH:	${OPENSSLSRC}/crypto/sha

SHA_SRCS += sha1dgst.c sha256.c sha512.c sha3.c
SHA_SRCS += libc-sha1.c libc-sha256.c libc-sha512.c
SHA_SRCS += keccak1600.c

SRCS += ${SHA_SRCS}

.for cryptosrc in ${SHA_SRCS}
CPPFLAGS.${cryptosrc} += -I${OPENSSLSRC}/crypto/sha ${SHACPPFLAGS}
.endfor
