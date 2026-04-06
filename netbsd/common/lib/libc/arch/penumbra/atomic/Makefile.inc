#	$NetBSD$

.if defined(LIB) && (${LIB} == "kern" || ${LIB} == "c" || ${LIB} == "pthread" \
	|| ${LIB} == "rump")

# Penumbra atomic CAS: interrupt-disable based (uniprocessor).
SRCS+=	atomic_cas.S

# Generic CAS-based implementations for all other operations.
SRCS+=	atomic_add_32_cas.c atomic_add_32_nv_cas.c \
	atomic_and_32_cas.c atomic_and_32_nv_cas.c \
	atomic_dec_32_cas.c atomic_dec_32_nv_cas.c \
	atomic_inc_32_cas.c atomic_inc_32_nv_cas.c \
	atomic_or_32_cas.c atomic_or_32_nv_cas.c \
	atomic_swap_32_cas.c

# Memory barriers are no-ops on uniprocessor with no store buffer.
SRCS+=	membar_ops_nop.c

.endif
