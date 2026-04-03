# Penumbra stage 1 bootloader — standalone Makefile
#
# Builds the stage 1 bootloader using the Penumbra clang toolchain.
# This is a standalone build (not using NetBSD build.sh) that compiles
# our boot sources plus selected libsa/libkern files from the NetBSD tree.
#
# Usage:
#   make                  — compile boot objects (link TBD)
#   make clean            — remove build artifacts
#   make LLVM_PREFIX=...  — override LLVM toolchain location

ROOT      := $(realpath ../../../../../..)
NBSDSRC   := $(ROOT)/netbsd
SYSDIR    := $(NBSDSRC)/sys
ARCHDIR   := $(SYSDIR)/arch/penumbra
BUILD     := $(ROOT)/build/boot1

# ── LLVM toolchain ────────────────────────────────────────────
LLVM_PREFIX ?= $(ROOT)/build/llvm
CC       = $(LLVM_PREFIX)/bin/clang --target=penumbra-unknown-none
LD       = $(LLVM_PREFIX)/bin/ld.lld
OBJCOPY  = $(LLVM_PREFIX)/bin/llvm-objcopy

# ── Compiler flags ────────────────────────────────────────────
# -D_STANDALONE enables standalone-mode types in NetBSD headers.
# The machine/ symlink is created at build time.
CFLAGS   = -O0 -ffreestanding -nostdinc -D_STANDALONE \
           -I$(SYSDIR) -MMD -MP

# ── Our source files ─────────────────────────────────────────
BOOT_SRCS = boot.c conf.c
LIBSA_SRCS = sdblk.c cons.c

BOOT_OBJS  = $(BOOT_SRCS:%.c=$(BUILD)/boot/%.o)
LIBSA_OBJS = $(LIBSA_SRCS:%.c=$(BUILD)/libsa/%.o)

OUR_OBJS   = $(BOOT_OBJS) $(LIBSA_OBJS)

# ── Targets ───────────────────────────────────────────────────
.PHONY: all clean machine-symlink

all: machine-symlink $(OUR_OBJS)
	@echo "Stage 1 objects built OK."

# Create machine/ symlink for <machine/*.h> resolution
machine-symlink:
	@ln -sfn arch/penumbra/include $(SYSDIR)/machine

$(BUILD)/boot/%.o: $(ARCHDIR)/stand/boot/%.c | $(BUILD)/boot
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/libsa/%.o: $(ARCHDIR)/stand/libsa/%.c | $(BUILD)/libsa
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/boot $(BUILD)/libsa:
	@mkdir -p $@

clean:
	rm -rf $(BUILD)
	rm -f $(SYSDIR)/machine

-include $(OUR_OBJS:.o=.d)
