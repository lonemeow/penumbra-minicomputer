# exhibit-launcher — primary-display menu for an exhibit machine
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  Static, like penmon: the exhibit
# image must not depend on the dynamic linker being healthy for its only
# user interface to come up.
#
# Output: build/exhibit-launcher/exhibit-launcher{,-static}

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

LDFLAGS_DYN := --target=$(TARGET) --sysroot=$(DESTDIR)
LDFLAGS_STA := --target=$(TARGET) --sysroot=$(DESTDIR) -static

BUILDDIR    := $(PROJROOT)/build/exhibit-launcher
OBJDIR      := $(BUILDDIR)/obj

PROG        := exhibit-launcher
SRCS        := main.c config.c monitor.c tty.c
OBJS        := $(addprefix $(OBJDIR)/,$(SRCS:.c=.o))

.PHONY: all dynamic static overlay clean
all: dynamic
dynamic: $(BUILDDIR)/$(PROG)
static:  $(BUILDDIR)/$(PROG)-static

# The launcher plus its configuration.  The config is data the exhibit
# operator edits in place, so it is installed under /usr/local/etc
# rather than compiled in.
overlay: static
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/$(PROG)-static \
	    $(OVERLAY_ROOT)/usr/local/bin/$(PROG)
	install -d $(OVERLAY_ROOT)/usr/local/etc
	install -m 0644 $(dir $(lastword $(MAKEFILE_LIST)))exhibit.conf \
	    $(OVERLAY_ROOT)/usr/local/etc/exhibit.conf

$(BUILDDIR)/$(PROG): $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(OBJS) -o $@

$(BUILDDIR)/$(PROG)-static: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(OBJS) -o $@

$(OBJDIR)/%.o: %.c exhibit.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR) $(OBJDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)
