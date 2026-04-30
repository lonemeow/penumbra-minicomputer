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

TARGET  := penumbra-unknown-none
COPT    ?= -O2
CFLAGS  := --target=$(TARGET) -fPIE $(COPT) -ffreestanding -nostdlib -I common
ASFLAGS := --target=$(TARGET) -fPIE -c
LDFLAGS := --pie --apply-dynamic-relocs --image-base=0 -T common/bench.ld

BUILDDIR := $(PROJROOT)/build/benchmark

# ── Targets ────────────────────────────────────────────────────

.PHONY: all dhrystone memtest membench clean

all: dhrystone memtest membench

# ── Common harness objects ─────────────────────────────────────

CRT0_OBJ   := $(BUILDDIR)/crt0.o
COMMON_OBJ := $(BUILDDIR)/bench.o $(BUILDDIR)/librt.o \
              $(BUILDDIR)/string.o $(BUILDDIR)/softfloat_stub.o

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(CRT0_OBJ): common/crt0.S | $(BUILDDIR)
	$(CC) $(ASFLAGS) -x assembler-with-cpp $< -o $@

$(BUILDDIR)/bench.o: common/bench.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/librt.o: common/librt.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/string.o: common/string.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/softfloat_stub.o: common/softfloat_stub.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

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

DHRYSTONE_ITERATIONS ?= 10000

# Stamp file: forces recompilation when iteration count changes.
# make doesn't track -D flag changes, so we write the current value
# to a file and depend on it.
DHRY_STAMP := $(BUILDDIR)/.dhry_iters
$(DHRY_STAMP): FORCE | $(BUILDDIR)
	@if [ ! -f $@ ] || [ "$$(cat $@)" != "$(DHRYSTONE_ITERATIONS)" ]; then \
		echo "$(DHRYSTONE_ITERATIONS)" > $@; \
	fi
FORCE:

DHRY_CFLAGS := $(CFLAGS) -isystem dhrystone/include \
               -DTIME -Dmain=dhrystone_main \
               -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) \
               -Wno-implicit-int -Wno-implicit-function-declaration \
               -Wno-return-type -Wno-deprecated-non-prototype

DHRY_OBJ := $(BUILDDIR)/dhry_1.o $(BUILDDIR)/dhry_2.o \
            $(BUILDDIR)/dhry_shim.o $(BUILDDIR)/dhry_entry.o

$(BUILDDIR)/dhry_1.o: dhrystone/dhry_1.c dhrystone/dhry.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_2.o: dhrystone/dhry_2.c dhrystone/dhry.h | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_shim.o: dhrystone/shim.c common/bench.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

$(BUILDDIR)/dhry_entry.o: dhrystone/entry.c common/bench.h $(DHRY_STAMP) | $(BUILDDIR)
	$(CC) $(CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

dhrystone: $(BUILDDIR)/DHRYSTON.ELF

$(BUILDDIR)/DHRYSTON.ELF: $(CRT0_OBJ) $(DHRY_OBJ) $(COMMON_OBJ) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(DHRY_OBJ) $(COMMON_OBJ) -o $@

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

$(BUILDDIR)/MEMTEST.ELF: $(CRT0_OBJ) $(MEMTEST_OBJ) $(COMMON_OBJ) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(MEMTEST_OBJ) $(COMMON_OBJ) -o $@

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

$(BUILDDIR)/MEMBENCH.ELF: $(CRT0_OBJ) $(MEMBENCH_OBJ) $(COMMON_OBJ) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(MEMBENCH_OBJ) $(COMMON_OBJ) -o $@

# ── Cleanup ────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
