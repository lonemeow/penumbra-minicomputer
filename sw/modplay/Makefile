# modplay — plays tracker modules through /dev/pmusic, or mixed in
# software through /dev/audio
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  The parser, replayer and mixer
# have no device in them, and also build for the host as modplay-dump,
# which walks a song without playing it.
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
COMMON      := $(PROJROOT)/sw/common

# <machine/penaudio.h> comes from the kernel source rather than the
# sysroot: the sysroot has it only once a distribution build installs
# it.  Only this one header is staged, so every other <machine/*.h>
# still resolves in the sysroot.
PENAUDIO_H  := $(PROJROOT)/netbsd/sys/arch/penumbra/include/penaudio.h

WARN        := -Wall -Wextra -Wno-unused-parameter
CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) $(WARN) \
               -I$(INCDIR) -I$(COMMON)
HOSTFLAGS   := -O2 $(WARN) -I$(COMMON)
LDFLAGS     := --target=$(TARGET) --sysroot=$(DESTDIR) -static
LDLIBS      := -lm

SONG_SRCS   := song.c mod.c pt.c replay.c
SONG_HDRS   := song.h mod.h replay.h
MIX_SRCS    := mixer.c
MIX_HDRS    := mixer.h $(COMMON)/fixed.h
SRCS        := modplay.c $(SONG_SRCS) $(MIX_SRCS)

.PHONY: all dump overlay test clean
all: $(BUILDDIR)/modplay
dump: $(BUILDDIR)/modplay-dump

overlay: all
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/modplay $(OVERLAY_ROOT)/usr/local/bin/modplay

$(BUILDDIR)/modplay: $(SRCS) $(SONG_HDRS) $(MIX_HDRS) \
    $(INCDIR)/machine/penaudio.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(SRCS) $(LDLIBS) -o $@

$(BUILDDIR)/modplay-dump: dump.c $(SONG_SRCS) $(SONG_HDRS) $(MIX_SRCS) \
    $(MIX_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) $(HOSTFLAGS) dump.c $(SONG_SRCS) $(MIX_SRCS) -lm -o $@

# Build and run the host unit tests for the parser, replayer and mixer.
test: $(BUILDDIR)/replay_test $(BUILDDIR)/mixer_test
	$(BUILDDIR)/replay_test
	$(BUILDDIR)/mixer_test

$(BUILDDIR)/replay_test: replay_test.c $(SONG_SRCS) $(SONG_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) $(HOSTFLAGS) replay_test.c $(SONG_SRCS) -lm -o $@

$(BUILDDIR)/mixer_test: mixer_test.c $(SONG_SRCS) $(SONG_HDRS) $(MIX_SRCS) \
    $(MIX_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) $(HOSTFLAGS) mixer_test.c $(SONG_SRCS) $(MIX_SRCS) -lm -o $@

$(INCDIR)/machine/penaudio.h: $(PENAUDIO_H)
	install -d $(dir $@)
	install -m 0644 $< $@

clean:
	rm -rf $(BUILDDIR)
