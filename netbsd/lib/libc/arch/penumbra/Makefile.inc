#	$NetBSD$

SRCS+=	__sigtramp2.S

CPPFLAGS+=	-I.

.if ${MKSOFTFLOAT} != "no"
CPPFLAGS+=	-DSOFTFLOAT
.include <softfloat/Makefile.inc>
.endif
