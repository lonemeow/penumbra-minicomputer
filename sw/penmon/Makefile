# penmon — Penumbra hardware-counter system monitor
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  No libcurses/libterminfo: the
# renderer (screen.c) emits ANSI directly, so the static binary has no
# terminfo-database dependency on the target.
#
# Output: build/penmon/penmon (dynamic), build/penmon/penmon-static

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2

CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) \
               -Wall -Wextra -Wno-unused-parameter \
               -I$(dir $(lastword $(MAKEFILE_LIST)))

LDLIBS      :=
LDFLAGS_DYN := --target=$(TARGET) --sysroot=$(DESTDIR)
LDFLAGS_STA := --target=$(TARGET) --sysroot=$(DESTDIR) -static

BUILDDIR    := $(PROJROOT)/build/penmon
OBJDIR      := $(BUILDDIR)/obj

SRCS        := penmon.c sample.c render.c proc.c screen.c
OBJS        := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))

# Host unit test for the screen damage-diff (no cross-toolchain needed).
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

# Build and run the screen diff test on the host.
test: $(BUILDDIR)/screen_test
	$(BUILDDIR)/screen_test

$(BUILDDIR)/penmon: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(OBJS) $(LDLIBS) -o $@

$(BUILDDIR)/penmon-static: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(OBJS) $(LDLIBS) -o $@

$(BUILDDIR)/screen_test: screen_test.c screen.c screen.h | $(BUILDDIR)
	$(HOSTCC) -Wall -Wextra -I. screen_test.c screen.c -o $@

$(OBJDIR)/%.o: %.c penmon.h screen.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR) $(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
