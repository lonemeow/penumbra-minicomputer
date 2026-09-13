# Skrolli Party 2026 entry: a bare-metal PIE ELF the ROM loads from FAT32.
#
#   make | make clean        LLVM_PREFIX=, COPT= override the defaults
#   boot sd:0,0/SKR2026.ELF  on the target

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
COPT        ?= -O2

CC      := $(LLVM_PREFIX)/bin/clang
LD      := $(LLVM_PREFIX)/bin/ld.lld
TARGET  := penumbra-unknown-none

# Lowering decisions are tuned per core, and the default tuning can move.
CPU     := penumbra2_5

# -MMD: penumbra.h lives outside this tree, and a stale object against it
# fails only at link time.
CFLAGS  := --target=$(TARGET) -mcpu=$(CPU) -fPIE $(COPT) \
           -ffreestanding -nostdlib \
           -Wall -Wextra -MMD -MP -I runtime -I $(PROJROOT)/hw/rom
LDFLAGS := --pie --apply-dynamic-relocs --image-base=0 -T runtime/demo.ld

BUILDDIR := $(PROJROOT)/build/skrolli2026

# 8.3: the ROM's FAT32 reader has no long names.
ELF      := $(BUILDDIR)/SKR2026.ELF

RT_SRCS := runtime/console.c runtime/display.c runtime/timer.c \
           runtime/perf.c runtime/telemetry.c runtime/director.c \
           runtime/part_testcard.c
SRCS    := main.c $(RT_SRCS)
OBJS    := $(addprefix $(BUILDDIR)/,$(notdir $(SRCS:.c=.o)))
CRT0    := $(BUILDDIR)/crt0.o

.PHONY: all clean stage
all: $(ELF)

$(BUILDDIR):
	@mkdir -p $@

$(CRT0): runtime/crt0.S | $(BUILDDIR)
	$(CC) --target=$(TARGET) -c $< -o $@

$(BUILDDIR)/%.o: runtime/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJS:.o=.d)

$(ELF): $(CRT0) $(OBJS) runtime/demo.ld
	$(LD) $(LDFLAGS) $(CRT0) $(OBJS) -o $@

# Copy the ELF into STAGE_DIR, for an SD image's FAT32 root.
stage: $(ELF)
	@test -n "$(STAGE_DIR)" || { echo "stage: STAGE_DIR is required" >&2; exit 1; }
	install -d $(STAGE_DIR)
	install -m 0644 $(ELF) $(STAGE_DIR)/

clean:
	rm -rf $(BUILDDIR)
