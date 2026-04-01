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
# Source discovery is automatic: all *.c files are compiled, crt0.s is
# the assembly startup stub.  Dependencies are tracked via .d files so
# only changed sources are recompiled.

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

CFLAGS   = -O0 -MMD -MP

# ── Source discovery ──────────────────────────────────────────
C_SRCS   := $(wildcard *.c)
C_OBJS   := $(C_SRCS:%.c=$(BUILD)/%.o)
ASM_OBJS := $(BUILD)/crt0.o
OBJS     := $(ASM_OBJS) $(C_OBJS)

# ── Targets ───────────────────────────────────────────────────
.PHONY: all clean

all: $(OUT_HEX)

$(OUT_HEX): $(BUILD)/boot_rom.bin
	@$(BIN2HEX) $< -o $@

$(BUILD)/boot_rom.bin: $(BUILD)/boot_rom.elf
	@$(OBJCOPY) -O binary $< $@

# Link order: crt0 first (entry point), then C objects
$(BUILD)/boot_rom.elf: $(OBJS) rom.ld
	@$(LD) -T rom.ld $(ASM_OBJS) $(C_OBJS) -o $@

# Pattern rule: C → object
$(BUILD)/%.o: %.c | $(BUILD)
	@$(CC) $(CFLAGS) -c -o $@ $<

# Assembly startup stub
$(BUILD)/crt0.o: crt0.s | $(BUILD)
	@$(MC) -filetype=obj $< -o $@

$(BUILD):
	@mkdir -p $@

clean:
	rm -rf $(BUILD) $(OUT_HEX)

# Pull in generated dependency files (header tracking)
-include $(C_OBJS:.o=.d)
