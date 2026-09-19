# pmusic-test — exercises /dev/pmusic, the audio device's voice interface
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.
#
# Output: build/pmusic-test/pmusic-test (static)

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2

BUILDDIR    := $(PROJROOT)/build/pmusic-test
INCDIR      := $(BUILDDIR)/include

# <machine/penaudio.h> comes from the kernel source rather than the
# sysroot: the sysroot has it only once a distribution build installs
# it, and the kernel under test is built from the source.  Only this
# one header is staged, so every other <machine/*.h> still resolves in
# the sysroot.
PENAUDIO_H  := $(PROJROOT)/netbsd/sys/arch/penumbra/include/penaudio.h

CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) \
               -Wall -Wextra -Wno-unused-parameter -I$(INCDIR)
LDFLAGS     := --target=$(TARGET) --sysroot=$(DESTDIR) -static
LDLIBS      := -lm

.PHONY: all overlay clean
all: $(BUILDDIR)/pmusic-test

overlay: all
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/pmusic-test \
	    $(OVERLAY_ROOT)/usr/local/bin/pmusic-test

$(BUILDDIR)/pmusic-test: pmusic-test.c $(INCDIR)/machine/penaudio.h
	$(CC) $(CFLAGS) $(LDFLAGS) pmusic-test.c $(LDLIBS) -o $@

$(INCDIR)/machine/penaudio.h: $(PENAUDIO_H)
	install -d $(dir $@)
	install -m 0644 $< $@

clean:
	rm -rf $(BUILDDIR)
