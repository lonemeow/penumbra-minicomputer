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

# Dhrystone-only: static link at fixed VA 0x80000000 so global accesses
# compile to the classic 2-instruction LUI+LDW sequence (no GOT).
# Paired with dhry_crt0.S which sets up the VA→PA mapping at boot.
DHRY_CFLAGS_BASE  := --target=$(TARGET) $(COPT) -ffreestanding -nostdlib -I common
DHRY_ASFLAGS      := --target=$(TARGET) -c
DHRY_LDFLAGS      := --image-base=0x80000000 -T dhrystone/dhry.ld

BUILDDIR := $(PROJROOT)/build/benchmark

# ── Targets ────────────────────────────────────────────────────

.PHONY: all dhrystone memtest membench strtest fbtest clean

# fbtest ships with the others so it is on the SD image ready to run
# on hardware; it stays out of BENCH_ELFS in the root Makefile, which
# is the list the automated ISS/RTL benchmark loop runs.
all: dhrystone memtest membench fbtest

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
# benchmarks and the static-linked Dhrystone build — the "rebuild as
# .dhry.o for GOT-free codegen" dance below does not apply to them.
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

# Dhrystone uses non-PIE codegen — the same C source files are
# rebuilt as *.dhry.o so the GOT-free LUI+LDW sequence reaches
# bench.c / librt.c too (otherwise common helpers would keep paying
# GOT indirection for every uart_putchar etc.).  The string routines
# need no .dhry.o variant: they reference no globals, so the shared
# $(PENSTRING_LIB) archive links cleanly at either image base.

DHRY_CFLAGS := $(DHRY_CFLAGS_BASE) -isystem dhrystone/include \
               -DTIME -Dmain=dhrystone_main \
               -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) \
               -Wno-implicit-int -Wno-implicit-function-declaration \
               -Wno-return-type -Wno-deprecated-non-prototype

DHRY_COMMON_CFLAGS := $(DHRY_CFLAGS_BASE)

DHRY_CRT0_OBJ := $(BUILDDIR)/dhry_crt0.o

DHRY_COMMON_OBJ := $(BUILDDIR)/bench.dhry.o $(BUILDDIR)/librt.dhry.o \
                   $(BUILDDIR)/softfloat_stub.dhry.o

DHRY_OBJ := $(BUILDDIR)/dhry_1.o $(BUILDDIR)/dhry_2.o \
            $(BUILDDIR)/dhry_shim.o $(BUILDDIR)/dhry_entry.o

$(DHRY_CRT0_OBJ): dhrystone/dhry_crt0.S | $(BUILDDIR)
	$(CC) $(DHRY_ASFLAGS) -x assembler-with-cpp $< -o $@

$(BUILDDIR)/bench.dhry.o: common/bench.c common/bench.h | $(BUILDDIR)
	$(CC) $(DHRY_COMMON_CFLAGS) -c $< -o $@

$(BUILDDIR)/librt.dhry.o: common/librt.c | $(BUILDDIR)
	$(CC) $(DHRY_COMMON_CFLAGS) -c $< -o $@

$(BUILDDIR)/softfloat_stub.dhry.o: common/softfloat_stub.c | $(BUILDDIR)
	$(CC) $(DHRY_COMMON_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_1.o: dhrystone/dhry_1.c dhrystone/dhry.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_2.o: dhrystone/dhry_2.c dhrystone/dhry.h | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_shim.o: dhrystone/shim.c common/bench.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(DHRY_COMMON_CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

$(BUILDDIR)/dhry_entry.o: dhrystone/entry.c common/bench.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(DHRY_COMMON_CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

dhrystone: $(BUILDDIR)/DHRYSTON.ELF

$(BUILDDIR)/DHRYSTON.ELF: $(DHRY_CRT0_OBJ) $(DHRY_OBJ) $(DHRY_COMMON_OBJ) $(PENSTRING_LIB) dhrystone/dhry.ld
	$(LD) $(DHRY_LDFLAGS) $(DHRY_CRT0_OBJ) $(DHRY_OBJ) $(DHRY_COMMON_OBJ) $(PENSTRING_LIB) -o $@

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

$(BUILDDIR)/MEMTEST.ELF: $(CRT0_OBJ) $(MEMTEST_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(MEMTEST_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) -o $@

# ── membench ─────────────────────────────────────────────────
#
# Memory throughput / latency: cached vs uncached × W/H/B × R/W.
# Useful baseline for measuring step-6 (open-row) impact.
#
# Build: make membench
# Output: build/benchmark/MEMBENCH.ELF (load via boot sd:0,0/MEMBENCH.ELF)

MEMBENCH_OBJ := $(BUILDDIR)/membench_entry.o

$(BUILDDIR)/membench_entry.o: membench/entry.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

membench: $(BUILDDIR)/MEMBENCH.ELF

$(BUILDDIR)/MEMBENCH.ELF: $(CRT0_OBJ) $(MEMBENCH_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(MEMBENCH_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) -o $@

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

$(BUILDDIR)/STRTEST.ELF: $(CRT0_OBJ) $(STRTEST_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(STRTEST_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) -o $@

# ── fbtest ───────────────────────────────────────────────────
#
# Hardware exercise (not a benchmark) for the display framebuffer:
# aperture readback at word/halfword/byte width — the byte-enable path
# no other device on this bus uses — plus a test pattern for the
# monitor and the uncached fill rate.  Needs a real display device, so
# it is run on hardware rather than under `make test`.
#
# Not in `all` / BENCH_ELFS — run on its own:
#   make fbtest && boot sd:0,0/FBTEST.ELF

FBTEST_OBJ := $(BUILDDIR)/fbtest_entry.o

$(BUILDDIR)/fbtest_entry.o: fbtest/entry.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

fbtest: $(BUILDDIR)/FBTEST.ELF

$(BUILDDIR)/FBTEST.ELF: $(CRT0_OBJ) $(FBTEST_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(FBTEST_OBJ) $(COMMON_OBJ) $(PENSTRING_LIB) -o $@

# ── Cleanup ────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
