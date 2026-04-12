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
LDFLAGS := --pie --image-base=0 -T common/bench.ld

BUILDDIR := $(PROJROOT)/build/benchmark

# ── Targets ────────────────────────────────────────────────────

.PHONY: all dhrystone clean

all: dhrystone

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

DHRYSTONE_ITERATIONS ?= 1000

DHRY_CFLAGS := $(CFLAGS) -isystem dhrystone/include \
               -DTIME -Dmain=dhrystone_main \
               -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) \
               -Wno-implicit-int -Wno-implicit-function-declaration \
               -Wno-return-type -Wno-deprecated-non-prototype

DHRY_OBJ := $(BUILDDIR)/dhry_1.o $(BUILDDIR)/dhry_2.o \
            $(BUILDDIR)/dhry_shim.o $(BUILDDIR)/dhry_entry.o

$(BUILDDIR)/dhry_1.o: dhrystone/dhry_1.c dhrystone/dhry.h | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_2.o: dhrystone/dhry_2.c dhrystone/dhry.h | $(BUILDDIR)
	$(CC) $(DHRY_CFLAGS) -c $< -o $@

$(BUILDDIR)/dhry_shim.o: dhrystone/shim.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

$(BUILDDIR)/dhry_entry.o: dhrystone/entry.c common/bench.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -DDHRYSTONE_ITERATIONS=$(DHRYSTONE_ITERATIONS) -c $< -o $@

dhrystone: $(BUILDDIR)/DHRYSTON.ELF

$(BUILDDIR)/DHRYSTON.ELF: $(CRT0_OBJ) $(DHRY_OBJ) $(COMMON_OBJ) common/bench.ld
	$(LD) $(LDFLAGS) $(CRT0_OBJ) $(DHRY_OBJ) $(COMMON_OBJ) -o $@

# ── Cleanup ────────────────────────────────────────────────────

clean:
	rm -rf $(BUILDDIR)
