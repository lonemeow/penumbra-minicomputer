# modplay — plays tracker modules through /dev/pmusic, or mixed in
# software through /dev/audio
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  The parser, replayer and mixer
# have no device in them, and also build for the host as modplay-dump,
# which walks a song without playing it.
#
# make test runs the unit tests on the host; make test-iss runs the same
# cases built for Penumbra, on the ISS.
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
OBJCOPY     := $(LLVM_PREFIX)/bin/llvm-objcopy
PYTHON      ?= python3

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

SONG_SRCS   := song.c mod.c pt.c s3m.c st3.c replay.c
SONG_HDRS   := song.h mod.h s3m.h replay.h
MIX_SRCS    := mixer.c
MIX_HDRS    := mixer.h $(COMMON)/fixed.h
SRCS        := modplay.c $(SONG_SRCS) $(MIX_SRCS)

.PHONY: all dump overlay test test-iss clean
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
test: $(BUILDDIR)/replay_test $(BUILDDIR)/s3m_test $(BUILDDIR)/mixer_test
	$(BUILDDIR)/replay_test
	@echo
	$(BUILDDIR)/s3m_test
	@echo
	$(BUILDDIR)/mixer_test

$(BUILDDIR)/replay_test: replay_test.c $(SONG_SRCS) $(SONG_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) $(HOSTFLAGS) replay_test.c $(SONG_SRCS) -lm -o $@

$(BUILDDIR)/s3m_test: s3m_test.c $(SONG_SRCS) $(SONG_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) $(HOSTFLAGS) s3m_test.c $(SONG_SRCS) -lm -o $@

$(BUILDDIR)/mixer_test: mixer_test.c $(SONG_SRCS) $(SONG_HDRS) $(MIX_SRCS) \
    $(MIX_HDRS)
	install -d $(BUILDDIR)
	$(HOSTCC) $(HOSTFLAGS) mixer_test.c $(SONG_SRCS) $(MIX_SRCS) -lm -o $@

# ── The tests on the machine itself ───────────────────────────
#
# The host tests say nothing about the machine's own word size, where a
# long is 32 bits, so the same cases also build for Penumbra and run on
# the ISS.  They run bare, on the harness the compiler tests use
# (test/compiler/harness): its libc stub, and iss_stub.c for the few
# libm calls the replayers' tables need.  The soft float and the 64-bit
# arithmetic come from the compiler-rt builtins.
ISS         := $(PROJROOT)/sw/sim/penumbra-iss
HARNESS     := $(PROJROOT)/test/compiler/harness
BUILTINS    := $(PROJROOT)/build/compiler-rt-builtins-O2/lib/linux/libclang_rt.builtins-penumbra.a
ISSDIR      := $(BUILDDIR)/iss
ISSBASE     := --target=penumbra-unknown-none $(COPT) -fhosted -fno-builtin \
               -nostdlib -nostdinc -I. -I$(COMMON) -I$(HARNESS) \
               -isystem $(shell $(CC) -print-resource-dir)/include
ISSFLAGS    := $(ISSBASE) $(WARN)
# The harness belongs to the compiler tests, and builds as they build it.
ISSHARNESS  := $(ISSBASE) -std=gnu89 -w
ISS_TESTS   := replay_test s3m_test mixer_test

# Fewer trials there: the same cases, at a pace the ISS can walk.
ISSFLAGS_mixer_test := -DTICKS=40 -DTRIALS=3

test-iss: $(ISS_TESTS:%=$(ISSDIR)/%.hex)
	@test -x $(ISS) || { echo "error: $(ISS) not built —" \
	    "build it with: make -C $(PROJROOT) sw/sim/penumbra-iss"; exit 1; }
	@for t in $(ISS_TESTS); do \
	    echo "== $$t on the ISS"; \
	    $(ISS) $(ISSDIR)/$$t.hex +hosted +quiet || exit 1; \
	    echo; \
	done

$(ISSDIR)/%.hex: %.c $(SONG_SRCS) $(MIX_SRCS) $(SONG_HDRS) $(MIX_HDRS) \
    iss_stub.c $(HARNESS)/crt0.S $(HARNESS)/libc_stub.c
	@test -f $(BUILTINS) || { echo "error: $(BUILTINS) not found —" \
	    "build it with: sw/tools/setup-compiler-rt.sh -O2"; exit 1; }
	install -d $(ISSDIR)
	$(CC) $(ISSHARNESS) -c $(HARNESS)/crt0.S -o $(ISSDIR)/crt0.o
	$(CC) $(ISSHARNESS) -c $(HARNESS)/libc_stub.c -o $(ISSDIR)/libc_stub.o
	$(CC) $(ISSFLAGS) $(ISSFLAGS_$*) -ffreestanding \
	    -T $(HARNESS)/test.ld $(ISSDIR)/crt0.o $(ISSDIR)/libc_stub.o \
	    iss_stub.c $< $(SONG_SRCS) $(MIX_SRCS) $(BUILTINS) \
	    -o $(ISSDIR)/$*.elf
	$(OBJCOPY) -O binary $(ISSDIR)/$*.elf $(ISSDIR)/$*.bin
	$(PYTHON) $(PROJROOT)/sw/tools/bin2hex.py $(ISSDIR)/$*.bin -o $@

$(INCDIR)/machine/penaudio.h: $(PENAUDIO_H)
	install -d $(dir $@)
	install -m 0644 $< $@

clean:
	rm -rf $(BUILDDIR)
