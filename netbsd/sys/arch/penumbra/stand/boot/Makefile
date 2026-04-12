#	$NetBSD$
#
# Penumbra boot loader (PIE ELF, loaded by ROM from FAT32)
#
# Build with nbmake-penumbra (requires tools built via build.sh):
#   nbmake-penumbra -C netbsd/sys/arch/penumbra/stand obj   # create objdir first
#   nbmake-penumbra -C netbsd/sys/arch/penumbra/stand/boot

PROG=		PENBOOT.ELF
SRCS=		crt0.S boot.c conf.c
# MD libsa glue — built alongside the bootloader
SRCS+=		sdblk.c cons.c
# Compiler-rt builtins (no hardware MUL)
SRCS+=		mulsi3.c

NOMAN=		# no manual page
NOSSP=		yes

# We are a standalone (freestanding) program, not a hosted one
LIBCRT0=
LIBCRTI=
LIBC=
LIBCRTBEGIN=
LIBCRTEND=

S=		${.CURDIR}/../../../..

.PATH:		${.CURDIR}/../libsa

CPPFLAGS+=	-nostdinc -D_STANDALONE -D__NetBSD__ -DHEAP_VARIABLE \
		-DLIBSA_SLOW_TWIDDLE -I${S} -I${.CURDIR}
CFLAGS+=	-O0 -ffreestanding -fPIE
CWARNFLAGS+=	-Wno-main

STRIPFLAG=
BINMODE=	444

CLEANFILES+=	${PROG}.sym

SAMISCMAKEFLAGS= SA_USE_LOADFILE=yes SA_INCLUDE_NET=no

# Custom link rule: must be defined BEFORE bsd.prog.mk so that its
# .if !commands(${_P}) check sees our rule and skips the default link.
# Output is a PIE ELF — no objcopy needed, just rename .sym → final.
${PROG}: ${PROG}.sym
	cp ${PROG}.sym ${PROG}

.include <bsd.prog.mk>

### find out what to use for libkern
KERN_AS=	library
.include "${S}/lib/libkern/Makefile.inc"
LIBKERN=	${KERNLIB}

### find out what to use for libsa
SA_AS=		library
.include "${S}/lib/libsa/Makefile.inc"
LIBSA=		${SALIB}

${PROG}.sym: ${OBJS} ${SALIB} ${KERNLIB}
	${_MKTARGET_LINK}
	${LD} --pie -T ${.CURDIR}/boot.ld -e _start -o ${PROG}.sym \
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
