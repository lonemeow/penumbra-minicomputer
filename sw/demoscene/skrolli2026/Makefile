# Skrolli Party 2026 entry: a bare-metal PIE ELF the ROM loads from FAT32.
#
#   make | make clean        LLVM_PREFIX=, COPT= override the defaults
#   make MOD=<file.mod>      link a song; without one the demo runs silent
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

RT_SRCS := runtime/console.c runtime/bootdata.c runtime/memory.c \
           runtime/display.c \
           runtime/timer.c runtime/perf.c runtime/telemetry.c \
           runtime/director.c runtime/audio.c runtime/modplay.c \
           runtime/part_testcard.c
SRCS    := main.c $(RT_SRCS)
OBJS    := $(addprefix $(BUILDDIR)/,$(notdir $(SRCS:.c=.o))) $(BUILDDIR)/song.o
CRT0    := $(BUILDDIR)/crt0.o

# The module is not part of this tree, so it is named at build time.  A
# relative path is taken against where make was invoked or the project
# root, whichever exists.
MOD     ?=
ifeq ($(strip $(MOD)),)
SONG_BIN     :=
SONG_ASFLAGS :=
else
MOD_FILE := $(firstword $(wildcard $(abspath $(MOD)) $(PROJROOT)/$(MOD)))
ifeq ($(MOD_FILE),)
$(error MOD=$(MOD): no such file)
endif
SONG_BIN     := $(BUILDDIR)/song.bin
SONG_ASFLAGS := -DSONG
endif

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

# MOD is a variable, not a file make can see change, so stamp it.
SONG_STAMP := $(BUILDDIR)/.song
$(SONG_STAMP): FORCE | $(BUILDDIR)
	@if [ ! -f $@ ] || [ "$$(cat $@)" != "$(MOD_FILE)" ]; then \
		echo "$(MOD_FILE)" > $@; \
	fi

$(BUILDDIR)/song.bin: tools/mod2song.py $(SONG_STAMP) $(MOD_FILE) | $(BUILDDIR)
	python3 tools/mod2song.py $(MOD_FILE) -o $@

# .incbin resolves against the include path.
$(BUILDDIR)/song.o: runtime/song.S $(SONG_BIN) $(SONG_STAMP) | $(BUILDDIR)
	$(CC) --target=$(TARGET) -c $(SONG_ASFLAGS) -I $(BUILDDIR) $< -o $@

.PHONY: FORCE
FORCE:

$(ELF): $(CRT0) $(OBJS) runtime/demo.ld
	$(LD) $(LDFLAGS) $(CRT0) $(OBJS) -o $@

# Copy the ELF into STAGE_DIR, for an SD image's FAT32 root.
stage: $(ELF)
	@test -n "$(STAGE_DIR)" || { echo "stage: STAGE_DIR is required" >&2; exit 1; }
	install -d $(STAGE_DIR)
	install -m 0644 $(ELF) $(STAGE_DIR)/

clean:
	rm -rf $(BUILDDIR)
