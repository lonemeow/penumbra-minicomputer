# ras-test — checks that a captured context never names a mid-sequence PC
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.
#
# Output: build/ras-test/ras-test (static)

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2

BUILDDIR    := $(PROJROOT)/build/ras-test

# Static: the program registers its own sequence, so it depends on
# nothing libc places or exports.
CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) \
               -Wall -Wextra -Wno-unused-parameter
LDFLAGS     := --target=$(TARGET) --sysroot=$(DESTDIR) -static

.PHONY: all overlay clean
all: $(BUILDDIR)/ras-test

overlay: all
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/ras-test \
	    $(OVERLAY_ROOT)/usr/local/bin/ras-test

$(BUILDDIR)/ras-test: ras-test.c | $(BUILDDIR)
	$(CC) $(CFLAGS) $(LDFLAGS) ras-test.c -o $@

$(BUILDDIR):
	install -d $@

clean:
	rm -rf $(BUILDDIR)
