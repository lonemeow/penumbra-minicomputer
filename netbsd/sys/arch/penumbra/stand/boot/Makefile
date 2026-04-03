#	$NetBSD$
#
# Penumbra stage 1 bootloader
#
# Can be built two ways:
#   1. Via build.sh: ./build.sh -U -m penumbra tools && nbmake-penumbra
#   2. Standalone:   make -f Makefile.standalone

PROG=		boot1
SRCS=		boot.c conf.c
# MD libsa glue — built alongside the bootloader
SRCS+=		sdblk.c cons.c

NOMAN=		# no manual page
NOSSP=		yes
NOPIE=		yes

# We are a standalone (freestanding) program, not a hosted one
LIBCRT0=
LIBCRTI=
LIBC=
LIBCRTBEGIN=
LIBCRTEND=

S=		${.CURDIR}/../../../..

.PATH:		${.CURDIR}/../libsa

CPPFLAGS+=	-nostdinc -D_STANDALONE -I${S}
CFLAGS+=	-O0 -ffreestanding

STRIPFLAG=
BINMODE=	444

.include <bsd.prog.mk>

# Pull in libkern for memcpy, strlen, etc.
KERN_AS=	library
.include "${S}/lib/libkern/Makefile.inc"
LIBKERN=	${KERNLIB}

# Pull in libsa for standalone I/O (dosfs, printf, alloc, etc.)
SA_AS=		library
SAMISCMAKEFLAGS+="SA_USE_LOADFILE=yes"
.include "${S}/lib/libsa/Makefile.inc"
LIBSA=		${SALIB}

LDADD+=		${LIBSA} ${LIBKERN}
DPADD+=		${LIBSA} ${LIBKERN}

# Stub out CRT objects the link expects
/usr/lib/crt0.o:
	true
/usr/lib/crtbegin.o:
	true
/usr/lib/crtend.o:
	true
