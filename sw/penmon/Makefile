# penmon — Penumbra hardware-counter system monitor
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  No libcurses/libterminfo: the
# screen layer under sw/common emits ANSI directly, so the static
# binary has no terminfo-database dependency on the target.
#
# Output: build/penmon/penmon (dynamic), build/penmon/penmon-static

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay
COMMON      := $(PROJROOT)/sw/common

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2

CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) \
               -Wall -Wextra -Wno-unused-parameter \
               -I$(dir $(lastword $(MAKEFILE_LIST))) -I$(COMMON)

LDLIBS      :=
LDFLAGS_DYN := --target=$(TARGET) --sysroot=$(DESTDIR)
LDFLAGS_STA := --target=$(TARGET) --sysroot=$(DESTDIR) -static

BUILDDIR    := $(PROJROOT)/build/penmon
OBJDIR      := $(BUILDDIR)/obj

SRCS        := penmon.c sample.c render.c proc.c
COMMON_SRCS := screen.c
OBJS        := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o) $(COMMON_SRCS:.c=.o))

# Host unit test for the renderer's readouts (no cross-toolchain needed).
HOSTCC      ?= cc

.PHONY: all dynamic static overlay test clean
all: dynamic
dynamic: $(BUILDDIR)/penmon
static:  $(BUILDDIR)/penmon-static

# Install the static binary into the shared overlay fake-root.  It is a
# self-contained ANSI renderer: no libcurses/libterminfo, hence no
# terminfo-database dependency — it runs on a minimal rootfs too.
overlay: static
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/penmon-static \
	    $(OVERLAY_ROOT)/usr/local/bin/penmon

# Build and run the host unit test for the renderer's fixed-point
# readouts.  The screen layer it draws through carries its own test,
# run by make -C sw/common test.
test: $(BUILDDIR)/render_test
	$(BUILDDIR)/render_test

$(BUILDDIR)/penmon: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(OBJS) $(LDLIBS) -o $@

$(BUILDDIR)/penmon-static: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(OBJS) $(LDLIBS) -o $@

$(BUILDDIR)/render_test: render_test.c render.c $(COMMON)/screen.c penmon.h \
    $(COMMON)/screen.h | $(BUILDDIR)
	$(HOSTCC) -Wall -Wextra -Wno-unused-parameter -I. -I$(COMMON) \
	    render_test.c render.c $(COMMON)/screen.c -o $@

$(OBJDIR)/%.o: %.c penmon.h $(COMMON)/screen.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/%.o: $(COMMON)/%.c $(COMMON)/screen.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR) $(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
