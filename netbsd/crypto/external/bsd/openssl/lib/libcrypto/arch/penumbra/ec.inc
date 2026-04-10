#	$NetBSD$
#
# Penumbra is 32-bit — no __uint128_t for 64-bit nist curve optimizations.
ECCPPFLAGS+= -DOPENSSL_NO_EC_NISTP_64_GCC_128

.include "../../ec.inc"
