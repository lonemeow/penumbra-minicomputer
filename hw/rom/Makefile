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

CFLAGS   ?= -O0 -MMD -MP

# ── Source discovery ──────────────────────────────────────────
C_SRCS   := $(wildcard *.c)
ASM_SRCS := $(wildcard *.s)
C_OBJS   := $(C_SRCS:%.c=$(BUILD)/%.o)
ASM_OBJS := $(ASM_SRCS:%.s=$(BUILD)/%.o)
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ── Targets ───────────────────────────────────────────────────
.PHONY: all clean FORCE

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

# Pattern rule: C → object
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# Pattern rule: assembly → object
$(BUILD)/%.o: %.s | $(BUILD)
	$(MC) -filetype=obj $< -o $@

$(BUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT_HEX)

# Pull in generated dependency files (header tracking)
-include $(C_OBJS:.o=.d)
