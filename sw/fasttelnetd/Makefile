# fasttelnetd — NetBSD's telnetd, with the loop that carries a session's
# output patched
#
# Compiles the sources under netbsd/libexec/telnetd where they are,
# all but telnetd.c: that one is copied into the build directory with
# telnetd-loop.patch applied, so the NetBSD tree stays as imported.
# What the patch changes, and why, is in its own header.
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.
#
# Output: build/fasttelnetd/fasttelnetd

HERE        := $(dir $(lastword $(MAKEFILE_LIST)))
PROJROOT    := $(realpath $(HERE)/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

TELNETD_SRC ?= $(PROJROOT)/netbsd/libexec/telnetd
LIBTELNET   := $(PROJROOT)/netbsd/lib/libtelnet
LOOP_PATCH  := $(HERE)telnetd-loop.patch

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2

# What netbsd/libexec/telnetd/Makefile builds telnetd with, less
# AUTHENTICATION, ENCRYPTION and KRB5.  A guest gives login(1) a
# password, and a daemon without the three loads no library beyond
# libterminfo and libutil for each connection it takes.
CPPFLAGS    := -DLINEMODE -DKLUDGELINEMODE -DDIAGNOSTICS \
               -DOLD_ENVIRON -DENV_HACK -DSECURELOGIN \
               -DSUPPORT_UTMP -DSUPPORT_UTMPX \
               -I$(PROJROOT)/netbsd/lib -I$(TELNETD_SRC)
CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT)
LDFLAGS     := --target=$(TARGET) --sysroot=$(DESTDIR)
LDLIBS      := -lterminfo -lutil

BUILDDIR    := $(PROJROOT)/build/fasttelnetd
OBJDIR      := $(BUILDDIR)/obj

# telnetd's sources as its own Makefile lists them, telnetd.c apart,
# and from libtelnet the one file whose functions telnetd still calls
# once authentication is out.
TELNETD_SRCS   := authenc.c global.c slc.c state.c sys_term.c termstat.c \
                  utility.c
LIBTELNET_SRCS := getent.c

TELNETD_OBJS   := $(addprefix $(OBJDIR)/,$(TELNETD_SRCS:.c=.o))
LIBTELNET_OBJS := $(addprefix $(OBJDIR)/,$(LIBTELNET_SRCS:.c=.o))
OBJS           := $(TELNETD_OBJS) $(LIBTELNET_OBJS) $(OBJDIR)/telnetd.o

HDRS           := $(wildcard $(TELNETD_SRC)/*.h)

.PHONY: all overlay clean
all: $(BUILDDIR)/fasttelnetd

# Install into the shared overlay fake-root, where inetd.conf of the
# guest image looks for it.
overlay: all
	install -d $(OVERLAY_ROOT)/usr/local/libexec
	install -m 0755 $(BUILDDIR)/fasttelnetd \
	    $(OVERLAY_ROOT)/usr/local/libexec/fasttelnetd

$(BUILDDIR)/fasttelnetd: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

# A patch that fails still leaves its output file, unpatched.  Make has
# to delete it, or the next run would find it up to date and build the
# daemon without the change.
.DELETE_ON_ERROR:

# No fuzz: the hunk's context is the loop the patch was written for, and
# has to match it line for line, wherever in the file the loop sits.
$(BUILDDIR)/telnetd.c: $(TELNETD_SRC)/telnetd.c $(LOOP_PATCH) | $(BUILDDIR)
	patch --fuzz=0 --no-backup-if-mismatch -o $@ $< $(LOOP_PATCH)

# Each object names the directory its source comes from, so telnetd.o
# has one rule and it reads the patched copy.
$(OBJDIR)/telnetd.o: $(BUILDDIR)/telnetd.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(TELNETD_OBJS): $(OBJDIR)/%.o: $(TELNETD_SRC)/%.c $(HDRS) | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(LIBTELNET_OBJS): $(OBJDIR)/%.o: $(LIBTELNET)/%.c | $(OBJDIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

$(BUILDDIR) $(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
