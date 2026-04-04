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

CPPFLAGS+=	-nostdinc -D_STANDALONE -D__NetBSD__ -I${S}
CFLAGS+=	-O0 -ffreestanding
CWARNFLAGS+=	-Wno-main

STRIPFLAG=
BINMODE=	444

SAMISCMAKEFLAGS= SA_USE_LOADFILE=yes SA_INCLUDE_NET=no

.include <bsd.prog.mk>

### find out what to use for libkern
KERN_AS=	library
.include "${S}/lib/libkern/Makefile.inc"
LIBKERN=	${KERNLIB}

### find out what to use for libsa
SA_AS=		library
.include "${S}/lib/libsa/Makefile.inc"
LIBSA=		${SALIB}

CLEANFILES+=	${PROG}.sym

# Custom link rule: build libraries first, then link
${PROG}: ${PROG}.sym
	${OBJCOPY} -O binary ${PROG}.sym ${PROG}

${PROG}.sym: ${OBJS} ${SALIB} ${KERNLIB}
	${_MKTARGET_LINK}
	${LD} -T ${.CURDIR}/boot.ld -o ${PROG}.sym \
	    ${OBJS} ${SALIB} ${KERNLIB}

# Stub out CRT objects the link expects
/usr/lib/crt0.o:
	true
/usr/lib/crtbegin.o:
	true
/usr/lib/crtend.o:
	true

cleandir: .WAIT cleandirlocal
cleandirlocal:
	-rm -rf lib
