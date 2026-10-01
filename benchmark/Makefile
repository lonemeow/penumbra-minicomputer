# Makefile — Penumbra bare-metal benchmark build system
#
# Builds benchmark PIE ELFs suitable for booting via the ROM:
#   boot sd:0,0/DHRYSTON.ELF
#
# Usage:
#   make                    — build all benchmarks
#   make dhrystone          — build one benchmark
#   make clean              — remove build artifacts

# ── Toolchain ──────────────────────────────────────────────────
PROJROOT := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm

CC      := $(LLVM_PREFIX)/bin/clang
LD      := $(LLVM_PREFIX)/bin/ld.lld
AR      := $(LLVM_PREFIX)/bin/llvm-ar

TARGET  := penumbra-unknown-none
COPT    ?= -O2

# Default (memtest, membench): PIE so the ROM ELF loader can drop the
# image at any RAM address and the bench harness self-relocates.
CFLAGS  := --target=$(TARGET) -fPIE $(COPT) -ffreestanding -nostdlib -I common
ASFLAGS := --target=$(TARGET) -fPIE -c
LDFLAGS := --pie --apply-dynamic-relocs --image-base=0 -T common/bench.ld

# Static link at fixed VA 0x80000000, for a benchmark whose score is
# compared with other machines': global accesses compile to the classic
# 2-instruction LUI+LDW sequence (no GOT), as they do in the builds
# published scores come from.  Paired with common/crt0_static.S, which
# sets up the VA→PA mapping at boot.
STATIC_CFLAGS  := --target=$(TARGET) $(COPT) -ffreestanding -nostdlib -I common
STATIC_ASFLAGS := --target=$(TARGET) -c
STATIC_LDFLAGS := --image-base=0x80000000 -T common/bench_static.ld

BUILDDIR := $(PROJROOT)/build/benchmark

# ── Targets ────────────────────────────────────────────────────

.PHONY: all dhrystone coremark memtest membench strtest fbtest audiotest clean

# fbtest and audiotest ship with the others so they are on the SD image
# ready to run on hardware; both stay out of BENCH_ELFS in the root
# Makefile, which is the list the automated ISS/RTL benchmark loop
# runs, because both need a real device and a human to judge them.
all: dhrystone coremark memtest membench fbtest audiotest

# ── Common harness objects ─────────────────────────────────────

CRT0_OBJ   := $(BUILDDIR)/crt0.o
COMMON_OBJ := $(BUILDDIR)/bench.o $(BUILDDIR)/librt.o \
              $(BUILDDIR)/softfloat_stub.o

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(CRT0_OBJ): common/crt0.S | $(BUILDDIR)
	$(CC) $(ASFLAGS) -x assembler-with-cpp $< -o $@

$(BUILDDIR)/bench.o: common/bench.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/librt.o: common/librt.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/softfloat_stub.o: common/softfloat_stub.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# A static-link benchmark takes the harness rebuilt as *.static.o, so
# the GOT-free LUI+LDW sequence reaches bench.c / librt.c too (otherwise
# the helpers would keep paying GOT indirection for every uart_putchar
# etc.).  The string routines need no such variant: they reference no
# globals, so the shared $(PENSTRING_LIB) archive links cleanly at
# either image base.
STATIC_CRT0_OBJ   := $(BUILDDIR)/crt0_static.o
STATIC_COMMON_OBJ := $(BUILDDIR)/bench.static.o $(BUILDDIR)/librt.static.o \
                     $(BUILDDIR)/softfloat_stub.static.o

$(STATIC_CRT0_OBJ): common/crt0_static.S | $(BUILDDIR)
	$(CC) $(STATIC_ASFLAGS) -x assembler-with-cpp $< -o $@

$(BUILDDIR)/bench.static.o: common/bench.c common/bench.h | $(BUILDDIR)
	$(CC) $(STATIC_CFLAGS) -c $< -o $@

$(BUILDDIR)/librt.static.o: common/librt.c | $(BUILDDIR)
	$(CC) $(STATIC_CFLAGS) -c $< -o $@

$(BUILDDIR)/softfloat_stub.static.o: common/softfloat_stub.c | $(BUILDDIR)
	$(CC) $(STATIC_CFLAGS) -c $< -o $@

# ── Optimized string/memory routines (shared with NetBSD) ──────
#
# Rather than maintain byte-loop fallbacks here, assemble the
# hand-optimized .S routines straight out of the NetBSD source tree —
# common/lib/libc, the copy already shared between the kernel and
# userland.  Referencing them in place (never copying) means any future
# tuning of those routines lands in the benchmarks automatically, with
# no duplicate to drift out of sync.
#
# The routines are position-independent leaf code (registers + relative
# branches, no global references), so one archive serves both the PIE
# benchmarks and the static-link ones — the "rebuild as .static.o for
# GOT-free codegen" dance above does not apply to them.
# Linking an archive (not a fixed object list) pulls in only the
# routines actually referenced, and a wildcard absorbs any new .S that
# NetBSD adds to the directory without touching this Makefile.

NETBSD_STRING_DIR  := $(PROJROOT)/netbsd/common/lib/libc/arch/penumbra/string
NETBSD_STRING_SRCS := $(wildcard $(NETBSD_STRING_DIR)/*.S)
NETBSD_STRING_OBJS := $(patsubst $(NETBSD_STRING_DIR)/%.S,\
                                 $(BUILDDIR)/penstring/%.o,$(NETBSD_STRING_SRCS))

# Pure assembly: no -fPIE needed (it changes no emitted bytes for these
# relocation-free leaf routines), so a single flag set covers both models.
STRING_ASFLAGS := --target=$(TARGET) -c

PENSTRING_LIB := $(BUILDDIR)/libpenstring.a

$(BUILDDIR)/penstring:
	mkdir -p $@

$(BUILDDIR)/penstring/%.o: $(NETBSD_STRING_DIR)/%.S | $(BUILDDIR)/penstring
	$(CC) $(STRING_ASFLAGS) -x assembler-with-cpp $< -o $@

$(PENSTRING_LIB): $(NETBSD_STRING_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

# ── Compiler runtime ───────────────────────────────────────────
#
# What the compiler leaves to its runtime on a 32-bit machine: 64-bit
# division above all, which lets the harness do its arithmetic in 64
# bits instead of arguing about what fits in 32.  These are the
# compiler-rt builtins sw/tools/setup-compiler-rt.sh builds, linked as
# an archive after the harness objects, so a benchmark takes only the
# routines it calls and librt.c's stay the ones used where both define
# one.  The -O2 build whatever COPT is: its routines serve the
# harness's bookkeeping, not the code a benchmark measures.

BUILTINS := $(PROJROOT)/build/compiler-rt-builtins-O2/lib/linux/libclang_rt.builtins-penumbra.a

$(BUILTINS):
	@echo "error: $@ not found — run sw/tools/setup-compiler-rt.sh" >&2; false

HARNESS_LIBS := $(PENSTRING_LIB) $(BUILTINS)

# ── Dhrystone 2.1 ─────────────────────────────────────────────
#
# Original source files (dhry.h, dhry_1.c, dhry_2.c) are unmodified
# from the 1988 Usenet posting by Reinhold P. Weicker.
#
# Adaptation via:
#   -isystem dhrystone/include   shim headers (stdio.h, sys/times.h, etc.)
#   -DTIME                       select time() path (simplest to shim)
#   -Dmain=dhrystone_main        rename main() so bench_main can wrap it
#   -DDHRYSTONE_ITERATIONS=N     iteration count (default 100)
#
# Source: https://github.com/Keith-S-Thompson/dhrystone (v2.1/)

DHRYSTONE_ITERATIONS ?= 100000

# Stamp file: forces recompilation when iteration count changes.
# make doesn't track -D flag changes, so we write the current value
# to a file and depend on it.
DHRY_STAMP := $(BUILDDIR)/.dhry_iters
$(DHRY_STAMP): FORCE | $(BUILDDIR)
	@if [ ! -f $@ ] || [ "$$(cat $@)" != "$(DHRYSTONE_ITERATIONS)" ]; then \
		echo "$(DHRYSTONE_ITERATIONS)" > $@; \
	fi
FORCE:

# Dhrystone is a static-link benchmark (see STATIC_CFLAGS).

DHRY_CFLAGS := $(STATIC_CFLAGS) -isystem dhrystone/include \
               -DTIME -Dmain=dhrystone_main \
               -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) \
               -Wno-implicit-int -Wno-implicit-function-declaration \
               -Wno-return-type -Wno-deprecated-non-prototype

# What the timed loop runs, and what only surrounds it.
DHRY_TIMED_OBJ := $(BUILDDIR)/dhry_1.o $(BUILDDIR)/dhry_2.o
DHRY_GLUE_OBJ  := $(BUILDDIR)/dhry_shim.o $(BUILDDIR)/dhry_entry.o

$(BUILDDIR)/dhry_1.o: dhrystone/dhry_1.c dhrystone/dhry.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_2.o: dhrystone/dhry_2.c dhrystone/dhry.h | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_shim.o: dhrystone/shim.c common/bench.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(STATIC_CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

$(BUILDDIR)/dhry_entry.o: dhrystone/entry.c common/bench.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(STATIC_CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

dhrystone: $(BUILDDIR)/DHRYSTON.ELF

# The link order is the code layout, and lld places an archive member
# where its archive stands on the command line.  So the string routines
# go directly after Dhrystone's own two files and ahead of the shims
# and the harness: everything the timed loop runs is one contiguous
# stretch.  A stretch no longer than the L1 I-cache puts no more lines
# in any set than the cache has ways, so the loop cannot evict itself,
# and nothing added to the harness can move the routines into the sets
# Dhrystone's code already fills.
$(BUILDDIR)/DHRYSTON.ELF: $(STATIC_CRT0_OBJ) $(DHRY_TIMED_OBJ) $(PENSTRING_LIB) $(DHRY_GLUE_OBJ) $(STATIC_COMMON_OBJ) $(BUILTINS) common/bench_static.ld
	$(LD) $(STATIC_LDFLAGS) $(STATIC_CRT0_OBJ) $(DHRY_TIMED_OBJ) $(PENSTRING_LIB) $(DHRY_GLUE_OBJ) $(STATIC_COMMON_OBJ) $(BUILTINS) -o $@

# ── CoreMark 1.0 ──────────────────────────────────────────────
#
# coremark/ holds EEMBC's sources unmodified, from
# https://github.com/eembc/coremark at 9d07b05: the newest revision
# whose own coremark.md5 matches its files (a comment fix to coremark.h
# after it left the list stale).  A score may be called CoreMark only
# if the files that list names are unchanged (LICENSE.md, and "Run
# Rules" in upstream's README), so the build checks them.
#
# coremark/penumbra/ is the port, which the same rules leave to the
# platform, and the harness's entry point.  The port takes its seeds
# and its iteration count at run time, so nothing here sizes the run.
#
# The run rules want every source built with the same flags and the
# flags reported, so one flag set serves the port and CoreMark alike
# and FLAGS_STR carries its optimization and code generation options
# into the report.
#
# A static-link benchmark (see STATIC_CFLAGS), without the soft-float
# stubs: the port keeps CoreMark's own arithmetic in integers, and a
# stub would turn a float that crept in into a silent zero.

COREMARK_FLAGS_STR := $(filter -O% -f% -m%,$(STATIC_CFLAGS))
COREMARK_CFLAGS    := $(STATIC_CFLAGS) -I coremark -I coremark/penumbra \
                      -DFLAGS_STR='"$(COREMARK_FLAGS_STR)"'

COREMARK_CORE := core_list_join core_main core_matrix core_state core_util
COREMARK_PORT := core_portme ee_printf entry
COREMARK_HDRS := coremark/coremark.h coremark/penumbra/core_portme.h \
                 coremark/penumbra/port.h common/bench.h
COREMARK_TIMED_OBJ := $(COREMARK_CORE:%=$(BUILDDIR)/coremark_%.o)
COREMARK_GLUE_OBJ  := $(COREMARK_PORT:%=$(BUILDDIR)/coremark_%.o)

COREMARK_COMMON_OBJ := $(BUILDDIR)/bench.static.o $(BUILDDIR)/librt.static.o

COREMARK_CHECKED := $(BUILDDIR)/.coremark_checked
$(COREMARK_CHECKED): coremark/coremark.md5 coremark/coremark.h \
                     $(COREMARK_CORE:%=coremark/%.c) | $(BUILDDIR)
	cd coremark && md5sum --quiet -c coremark.md5
	@touch $@

$(BUILDDIR)/coremark_%.o: coremark/%.c $(COREMARK_HDRS) $(COREMARK_CHECKED) | $(BUILDDIR)
	$(CC) $(COREMARK_CFLAGS) -c $< -o $@

$(BUILDDIR)/coremark_%.o: coremark/penumbra/%.c $(COREMARK_HDRS) | $(BUILDDIR)
	$(CC) $(COREMARK_CFLAGS) -c $< -o $@

coremark: $(BUILDDIR)/COREMARK.ELF

# Laid out as Dhrystone is: CoreMark's own code, the string routines,
# then the port and the harness, so neither of those moves the code the
# timed region runs.  That code outgrows the L1 I-cache on the machines
# so far, so here the order buys a layout that holds still, not one
# free of conflicts.
$(BUILDDIR)/COREMARK.ELF: $(STATIC_CRT0_OBJ) $(COREMARK_TIMED_OBJ) $(PENSTRING_LIB) $(COREMARK_GLUE_OBJ) $(COREMARK_COMMON_OBJ) $(BUILTINS) common/bench_static.ld
	$(LD) $(STATIC_LDFLAGS) $(STATIC_CRT0_OBJ) $(COREMARK_TIMED_OBJ) $(PENSTRING_LIB) $(COREMARK_GLUE_OBJ) $(COREMARK_COMMON_OBJ) $(BUILTINS) -o $@

# ── memtest ──────────────────────────────────────────────────
#
# Memory correctness tester: round-trip / walking-1 / walking-0 /
# complement / sub-word / address-as-data patterns.  Walks a window
# of RAM discovered from bootdata.
#
# Build: make memtest
# Output: build/benchmark/MEMTEST.ELF (load via boot sd:0,0/MEMTEST.ELF)

MEMTEST_OBJ := $(BUILDDIR)/memtest_entry.o $(BUILDDIR)/memtest_patterns.o

$(BUILDDIR)/memtest_entry.o: memtest/entry.c memtest/patterns.h common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -Imemtest -c $< -o $@

$(BUILDDIR)/memtest_patterns.o: memtest/patterns.c memtest/patterns.h common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -Imemtest -c $< -o $@

memtest: $(BUILDDIR)/MEMTEST.ELF

$(BUILDDIR)/MEMTEST.ELF: $(CRT0_OBJ) $(MEMTEST_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(MEMTEST_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) -o $@

# ── membench ─────────────────────────────────────────────────
#
# Memory throughput / latency: cached vs uncached × W/H/B × R/W.
# Useful baseline for measuring step-6 (open-row) impact.
#
# Build: make membench
# Output: build/benchmark/MEMBENCH.ELF (load via boot sd:0,0/MEMBENCH.ELF)

# The access loops are hand-written so the instruction stream stays
# fixed across compiler changes — see membench/loops.S.
MEMBENCH_OBJ := $(BUILDDIR)/membench_entry.o $(BUILDDIR)/membench_loops.o

$(BUILDDIR)/membench_entry.o: membench/entry.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/membench_loops.o: membench/loops.S | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

membench: $(BUILDDIR)/MEMBENCH.ELF

$(BUILDDIR)/MEMBENCH.ELF: $(CRT0_OBJ) $(MEMBENCH_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(MEMBENCH_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) -o $@

# ── strtest ──────────────────────────────────────────────────
#
# Correctness test (not a benchmark) for the shared common/lib/libc
# string routines pulled from libpenstring.a.  Exercises strlen/strcmp/
# strcpy across every start alignment and length.  -fno-builtin forces
# real calls into the routines instead of clang-expanded builtins.
#
# Not in `all` / BENCH_ELFS — run on its own:
#   make strtest && boot sd:0,0/STRTEST.ELF   (with +halt-on-break on ISS)

STRTEST_OBJ := $(BUILDDIR)/strtest_entry.o

$(BUILDDIR)/strtest_entry.o: strtest/entry.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -fno-builtin -c $< -o $@

strtest: $(BUILDDIR)/STRTEST.ELF

$(BUILDDIR)/STRTEST.ELF: $(CRT0_OBJ) $(STRTEST_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(STRTEST_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) -o $@

# ── fbtest ───────────────────────────────────────────────────
#
# Hardware exercise (not a benchmark) for the display framebuffer:
# aperture readback at word/halfword/byte width — the byte-enable path
# no other device on this bus uses — a test pattern for the monitor,
# and a characterization of what reaching the aperture costs at each
# access width and under both cache mappings.  Needs a real display
# device, and the cost figures need real bus and cache timing, so it is
# run on hardware rather than under `make test`.
#
# Not in `all` / BENCH_ELFS — run on its own:
#   make fbtest && boot sd:0,0/FBTEST.ELF

# bandwidth.c reads the cache register map from the ROM's penumbra.h
# rather than restating it, so CACHE_INFO decoding lives in one place.
FBTEST_CFLAGS := $(CFLAGS) -I ../hw/rom
FBTEST_DEPS   := fbtest/display.h fbtest/bandwidth.h common/bench.h
FBTEST_OBJ    := $(BUILDDIR)/fbtest_entry.o $(BUILDDIR)/fbtest_bandwidth.o

$(BUILDDIR)/fbtest_entry.o: fbtest/entry.c $(FBTEST_DEPS) | $(BUILDDIR)
	$(CC) $(FBTEST_CFLAGS) -c $< -o $@

$(BUILDDIR)/fbtest_bandwidth.o: fbtest/bandwidth.c $(FBTEST_DEPS) | $(BUILDDIR)
	$(CC) $(FBTEST_CFLAGS) -c $< -o $@

fbtest: $(BUILDDIR)/FBTEST.ELF

$(BUILDDIR)/fbtest_tlbrange.o: common/tlbrange.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/FBTEST.ELF: $(CRT0_OBJ) $(FBTEST_OBJ) $(BUILDDIR)/fbtest_tlbrange.o $(COMMON_OBJ) $(HARNESS_LIBS) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(FBTEST_OBJ) $(BUILDDIR)/fbtest_tlbrange.o $(COMMON_OBJ) $(HARNESS_LIBS) -o $@

# ── audiotest ────────────────────────────────────────────────
#
# Hardware exercise (not a benchmark) for the CLASS_AUDIO device: the
# register protocol first, then real material to listen to.  The output
# stage was characterised on the bring-up probe, but nothing has driven
# the device from the CPU, so the FIFO port and the frame handoff to
# the output clock are only as proven as the first hardware run makes
# them.  Needs a real device and a pair of ears, so it is run on
# hardware rather than under `make test`.
#
# Ships in `all` so it reaches the SD image; stays out of BENCH_ELFS,
# which is the automated ISS/RTL loop:
#   boot sd:0,0/AUDIOTES.ELF
#
# The listening half needs material, and material is the user's own
# file rather than this tree's — nothing audio is ever committed here.
# So the default build links no clip at all and the program falls back
# to a test tone, which still proves the whole path end to end.  Point
# AUDIO_WAV at anything to hear it instead:
#
#   make audiotest AUDIO_WAV=~/music/whatever.wav
#
# Any format `wave` reads works; it is folded to stereo and resampled
# to the device rate.  AUDIOTEST_FRAMES caps the length — 4 bytes a
# frame, riding inside the ELF, so the default is ~2 MB / 10.9 s at
# 48 kHz.
AUDIOTEST_FRAMES ?= 524288
AUDIO_WAV        ?=

AUDIOTEST_OBJ := $(BUILDDIR)/audiotest_entry.o $(BUILDDIR)/audiotest_sample.o

# With no source material there is no blob and sample.S assembles to an
# empty span, which entry.c reads as "generate a tone instead".
ifeq ($(strip $(AUDIO_WAV)),)
AUDIOTEST_CLIP    :=
AUDIOTEST_WAV     :=
AUDIOTEST_ASFLAGS := $(ASFLAGS)
else
# A relative AUDIO_WAV is nearly always relative to where make was
# invoked — the project root — while this makefile runs in benchmark/.
# Accept either, and fail with the paths tried rather than as a missing
# make target.
AUDIOTEST_WAV := $(firstword $(wildcard $(abspath $(AUDIO_WAV)) \
                                        $(PROJROOT)/$(AUDIO_WAV)))
ifeq ($(AUDIOTEST_WAV),)
$(error AUDIO_WAV=$(AUDIO_WAV): no such file — tried $(abspath $(AUDIO_WAV)) \
and $(PROJROOT)/$(AUDIO_WAV))
endif
AUDIOTEST_CLIP    := $(BUILDDIR)/audio_sample.bin
AUDIOTEST_ASFLAGS := $(ASFLAGS) -DAUDIO_CLIP
endif

# Neither the length nor the source file is a prerequisite make can see
# (one is a variable, the other may be empty), so stamp both and depend
# on the stamp.  Same reason as $(DHRY_STAMP).
AUDIOTEST_STAMP := $(BUILDDIR)/.audiotest_clip
$(AUDIOTEST_STAMP): FORCE | $(BUILDDIR)
	@if [ ! -f $@ ] || [ "$$(cat $@)" != "$(AUDIOTEST_FRAMES) $(AUDIOTEST_WAV)" ]; then \
		echo "$(AUDIOTEST_FRAMES) $(AUDIOTEST_WAV)" > $@; \
	fi

$(BUILDDIR)/audio_sample.bin: ../hw/tools/wav2sample.py $(AUDIOTEST_STAMP) \
                              $(AUDIOTEST_WAV) | $(BUILDDIR)
	python3 ../hw/tools/wav2sample.py $(AUDIOTEST_WAV) -o $@ \
		-f bin -c 2 -n $(AUDIOTEST_FRAMES)

$(BUILDDIR)/audiotest_entry.o: audiotest/entry.c audiotest/audio.h \
                               common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# .incbin resolves against the assembler's include path, so the source
# names the blob and -I says where the build put it.  The stamp is a
# prerequisite because toggling AUDIO_WAV changes -DAUDIO_CLIP without
# changing sample.S.
$(BUILDDIR)/audiotest_sample.o: audiotest/sample.S $(AUDIOTEST_CLIP) \
                                $(AUDIOTEST_STAMP) | $(BUILDDIR)
	$(CC) $(AUDIOTEST_ASFLAGS) -x assembler-with-cpp -I $(BUILDDIR) $< -o $@

audiotest: $(BUILDDIR)/AUDIOTES.ELF

$(BUILDDIR)/AUDIOTES.ELF: $(CRT0_OBJ) $(AUDIOTEST_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(AUDIOTEST_OBJ) $(COMMON_OBJ) $(HARNESS_LIBS) -o $@

# ── Cleanup ────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
