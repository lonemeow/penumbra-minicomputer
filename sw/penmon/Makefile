# penmon — Penumbra hardware-counter system monitor
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  Links libcurses + libterminfo.
#
# Runtime note: curses needs the compiled terminfo database on the
# target at /usr/share/misc/terminfo.cdb and TERM set (e.g.
# xterm-256color).  See the README for deploy details.
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

LDLIBS      := -lcurses -lterminfo
LDFLAGS_DYN := --target=$(TARGET) --sysroot=$(DESTDIR)
LDFLAGS_STA := --target=$(TARGET) --sysroot=$(DESTDIR) -static

BUILDDIR    := $(PROJROOT)/build/penmon
OBJDIR      := $(BUILDDIR)/obj

SRCS        := penmon.c sample.c render.c proc.c
OBJS        := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))

.PHONY: all dynamic static overlay clean
all: dynamic
dynamic: $(BUILDDIR)/penmon
static:  $(BUILDDIR)/penmon-static

# Install the static binary (no libcurses/libterminfo needed on the
# target) into the shared overlay fake-root.  Only a non-base path is
# touched: the terminfo database curses needs is a base-system file,
# already present on a full rootfs (ROOTFS_FULL=1) at
# /usr/share/misc/terminfo.cdb — overlaying it here would collide with
# the distribution's own METALOG entry.
overlay: static
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/penmon-static \
	    $(OVERLAY_ROOT)/usr/local/bin/penmon

$(BUILDDIR)/penmon: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(OBJS) $(LDLIBS) -o $@

$(BUILDDIR)/penmon-static: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(OBJS) $(LDLIBS) -o $@

$(OBJDIR)/%.o: %.c penmon.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR) $(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
