#	$NetBSD: Makefile,v 1.11.2.1 2025/04/14 09:58:17 martin Exp $

.include <bsd.hostinit.mk>

.include "${.CURDIR}/../Makefile.ctftools"

HOSTPROGNAME=   ${_TOOL_PREFIX}ctfmerge
HOST_SRCDIR=    external/cddl/osnet/usr.bin/ctfmerge

# need native pthread support
HOST_CFLAGS+=	-pthread

.include "${.CURDIR}/../Makefile.nbincludes"
.include "${.CURDIR}/../Makefile.host"
