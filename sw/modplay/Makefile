# modplay — plays ProTracker modules through /dev/pmusic
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  The parser and replayer have no
# device in them, and also build for the host as modplay-dump, which
# walks a song without playing it.
#
# Output: build/modplay/modplay (static), build/modplay/modplay-dump (host)

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2
HOSTCC      ?= cc

BUILDDIR    := $(PROJROOT)/build/modplay
INCDIR      := $(BUILDDIR)/include

# <machine/penaudio.h> comes from the kernel source rather than the
# sysroot: the sysroot has it only once a distribution build installs
# it.  Only this one header is staged, so every other <machine/*.h>
# still resolves in the sysroot.
PENAUDIO_H  := $(PROJROOT)/netbsd/sys/arch/penumbra/include/penaudio.h

WARN        := -Wall -Wextra -Wno-unused-parameter
CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) $(WARN) \
               -I$(INCDIR)
LDFLAGS     := --target=$(TARGET) --sysroot=$(DESTDIR) -static
LDLIBS      := -lm

SONG_SRCS   := song.c mod.c pt.c replay.c
SONG_HDRS   := song.h mod.h replay.h
SRCS        := modplay.c $(SONG_SRCS)

.PHONY: all dump overlay test clean
all: $(BUILDDIR)/modplay
dump: $(BUILDDIR)/modplay-dump

overlay: all
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/modplay $(OVERLAY_ROOT)/usr/local/bin/modplay

$(BUILDDIR)/modplay: $(SRCS) $(SONG_HDRS) $(INCDIR)/machine/penaudio.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) $(LDLIBS) -o $@

$(BUILDDIR)/modplay-dump: dump.c $(SONG_SRCS) $(SONG_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) -O2 $(WARN) dump.c $(SONG_SRCS) -lm -o $@

# Build and run the host unit test for the parser and replayer.
test: $(BUILDDIR)/replay_test
	$(BUILDDIR)/replay_test

$(BUILDDIR)/replay_test: replay_test.c $(SONG_SRCS) $(SONG_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) -O2 $(WARN) replay_test.c $(SONG_SRCS) -lm -o $@

$(INCDIR)/machine/penaudio.h: $(PENAUDIO_H)
	install -d $(dir $@)
	install -m 0644 $< $@

clean:
	rm -rf $(BUILDDIR)
