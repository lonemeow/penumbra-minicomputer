# hw/rom/Makefile — Penumbra boot ROM build
#
# Builds the C boot ROM into program.hex (placed in the project root
# for $readmemh pickup by the simulator).
#
# Usage:
#   make                  — build program.hex
#   make clean            — remove build artifacts
#   make LLVM_PREFIX=...  — override LLVM toolchain location
#
# Source discovery is automatic: all *.c and *.s files are compiled.
# Dependencies are tracked via .d files so only changed sources are
# recompiled.

ROOT      := $(realpath ../..)
BUILD     := $(ROOT)/build/rom
OUT_HEX   := $(ROOT)/program.hex

# ── LLVM toolchain ────────────────────────────────────────────
LLVM_PREFIX ?= $(ROOT)/build/llvm
CC       = $(LLVM_PREFIX)/bin/clang --target=penumbra-unknown-none
MC       = $(LLVM_PREFIX)/bin/llvm-mc -triple=penumbra
LD       = $(LLVM_PREFIX)/bin/ld.lld
OBJCOPY  = $(LLVM_PREFIX)/bin/llvm-objcopy
BIN2HEX  = python3 $(ROOT)/sw/tools/bin2hex.py

CFLAGS   ?= -O0
# What the build needs whatever CFLAGS says, so a CFLAGS from the
# command line (the top-level Makefile's included) cannot drop it:
# dependency files, and the build directory, where the generated
# headers (splash art) live.
ROM_CFLAGS := -MMD -MP -I$(BUILD)

# ── Generated sources ─────────────────────────────────────────
# The splash screen is authored as text and transcoded to CP437 rows;
# the ROM draws it into a display device's cells when it finds one.
SPLASH_TXT := $(ROOT)/hw/video/splash.txt
SPLASH_H   := $(BUILD)/splash_art.h
GEN_HDRS   := $(SPLASH_H)

# ── Source discovery ──────────────────────────────────────────
C_SRCS   := $(wildcard *.c)
ASM_SRCS := $(wildcard *.s)
C_OBJS   := $(C_SRCS:%.c=$(BUILD)/%.o)
ASM_OBJS := $(ASM_SRCS:%.s=$(BUILD)/%.o)
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ── Targets ───────────────────────────────────────────────────
.PHONY: all clean test FORCE

all: $(OUT_HEX)

# OUT_HEX lives at the project root and is also written by other build
# targets (pasm.py for hw/sim tests, benchmarks).  Force-regenerate from
# our bin every invocation so a stale third-party program.hex with a
# newer mtime can't fool make into skipping the bin2hex step.
$(OUT_HEX): $(BUILD)/boot_rom.bin FORCE
	$(BIN2HEX) $< -o $@

FORCE:

$(BUILD)/boot_rom.bin: $(BUILD)/boot_rom.elf
	$(OBJCOPY) -O binary $< $@

# Link: crt0 first (entry point), then remaining asm, then C objects.
# Linker script places .text._start first regardless, but this keeps
# the link order predictable.
$(BUILD)/boot_rom.elf: $(OBJS) rom.ld
	$(LD) -T rom.ld $(BUILD)/crt0.o $(filter-out $(BUILD)/crt0.o,$(ASM_OBJS)) $(C_OBJS) -o $@

$(SPLASH_H): $(SPLASH_TXT) $(ROOT)/hw/tools/text2cells.py | $(BUILD)
	python3 $(ROOT)/hw/tools/text2cells.py --format c $< $@

# Pattern rule: C → object
$(BUILD)/%.o: %.c $(GEN_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(ROM_CFLAGS) -c -o $@ $<

# Pattern rule: assembly → object
$(BUILD)/%.o: %.s | $(BUILD)
	$(MC) -filetype=obj $< -o $@

$(BUILD):
	@mkdir -p $@

# ── Host unit tests ───────────────────────────────────────────
# fat32.c reaches the disk only through its blk_read_fn callback, so
# it builds and runs natively against a synthesized image — no
# simulator, no cross toolchain, no SD card.
HOST_CC  ?= cc
TEST_BIN := $(BUILD)/fat32_test

test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): tests/fat32_test.c fat32.c util.c fat32.h util.h penumbra.h | $(BUILD)
	$(HOST_CC) -std=gnu11 -Wall -Wextra -O1 -g -o $@ \
	    tests/fat32_test.c fat32.c util.c

clean:
	rm -rf $(BUILD) $(OUT_HEX)

# Pull in generated dependency files (header tracking)
-include $(C_OBJS:.o=.d)
