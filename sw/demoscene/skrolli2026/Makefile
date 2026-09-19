# Skrolli Party 2026 entry: a bare-metal PIE ELF the ROM loads from FAT32.
#
#   make | make clean        LLVM_PREFIX=, COPT= override the defaults
#   make MOD=<file.mod>      link the song; without one the demo panics at start
#   boot sd:0,0/SKR2026.ELF  on the target

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
COPT        ?= -O2

CC      := $(LLVM_PREFIX)/bin/clang
LD      := $(LLVM_PREFIX)/bin/ld.lld
AR      := $(LLVM_PREFIX)/bin/llvm-ar
TARGET  := penumbra-unknown-none

# Lowering decisions are tuned per core, and the default tuning can move.
CPU     := penumbra2_5

# -MMD: penumbra.h lives outside this tree, and a stale object against it
# fails only at link time.  The demo is one static PIE, so every global
# resolves inside it: -fdirect-access-external-data addresses them
# PC-relative instead of loading each address from the GOT.
CFLAGS  := --target=$(TARGET) -mcpu=$(CPU) -fPIE $(COPT) \
           -fdirect-access-external-data \
           -ffreestanding -nostdlib \
           -Wall -Wextra -MMD -MP -I runtime -I $(PROJROOT)/hw/rom
LDFLAGS := --pie --apply-dynamic-relocs --image-base=0 -T runtime/demo.ld

BUILDDIR := $(PROJROOT)/build/skrolli2026

# Generated sources: the telemetry digits and each part's typed text.
CFLAGS  += -I $(BUILDDIR)/gen

# 8.3: the ROM's FAT32 reader has no long names.
ELF      := $(BUILDDIR)/SKR2026.ELF

RT_SRCS := runtime/console.c runtime/bootdata.c runtime/memory.c \
           runtime/fix16.c \
           runtime/display.c \
           runtime/timer.c runtime/perf.c runtime/telemetry.c \
           runtime/director.c runtime/audio.c runtime/modplay.c \
           runtime/typer.c runtime/image.c
# Every part in parts/ is built; main.c's cue table decides what runs.
PART_SRCS := $(wildcard parts/*.c)

# parts/<name>.txt becomes <name>_text.h, checked as it is compiled.
TEXT_SRCS := $(wildcard parts/*.txt)
TEXT_HDRS := $(patsubst parts/%.txt,$(BUILDDIR)/gen/%_text.h,$(TEXT_SRCS))

# assets/<name>.png becomes <name>_image.c and .h, checked likewise.
IMAGE_SRCS := $(wildcard assets/*.png)
IMAGE_HDRS := $(patsubst assets/%.png,$(BUILDDIR)/gen/%_image.h,$(IMAGE_SRCS))

SRCS    := main.c $(RT_SRCS) $(PART_SRCS)
# Objects mirror the source tree, so parts and runtime may share a name.
OBJS    := $(addprefix $(BUILDDIR)/,$(SRCS:.c=.o)) $(BUILDDIR)/song.o \
           $(BUILDDIR)/gen/telefont.o $(BUILDDIR)/gen/sintab.o \
           $(IMAGE_HDRS:.h=.o)

# The telemetry text's glyphs, cut from the font the display itself
# shows, built the way the top-level Makefile builds it.
FONT_SRC := $(PROJROOT)/netbsd/sys/dev/wsfont/bold8x16.h
FONT_HEX := $(BUILDDIR)/font8x16.hex
CRT0    := $(BUILDDIR)/crt0.o

# The machine's tuned string routines, shared with NetBSD's libc.  An
# archive, so only the ones a build references are linked.
LIBC_DIR   := $(PROJROOT)/netbsd/common/lib/libc/arch/penumbra/string
LIBC_FUNCS := memcpy memset memcmp strlen strcmp strcpy
LIBC_OBJS  := $(addprefix $(BUILDDIR)/libc/,$(addsuffix .o,$(LIBC_FUNCS)))
LIBC       := $(BUILDDIR)/libc.a

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

$(BUILDDIR)/libc/%.o: $(LIBC_DIR)/%.S | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) --target=$(TARGET) -c $< -o $@

$(LIBC): $(LIBC_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(FONT_HEX): $(FONT_SRC) $(PROJROOT)/hw/tools/wsfont2hex.py | $(BUILDDIR)
	python3 $(PROJROOT)/hw/tools/wsfont2hex.py $< $@

$(BUILDDIR)/gen/telefont.c: tools/telefont.py $(FONT_HEX)
	@mkdir -p $(dir $@)
	python3 tools/telefont.py $(FONT_HEX) -o $@

$(BUILDDIR)/gen/%_text.h: parts/%.txt tools/text2typer.py
	@mkdir -p $(dir $@)
	python3 tools/text2typer.py $< -o $@

# One run writes both files.
$(BUILDDIR)/gen/%_image.c $(BUILDDIR)/gen/%_image.h: assets/%.png tools/png2image.py
	@mkdir -p $(dir $@)
	python3 tools/png2image.py $< -o $(BUILDDIR)/gen/$*_image.c

$(BUILDDIR)/gen/%_image.o: $(BUILDDIR)/gen/%_image.c
	$(CC) $(CFLAGS) -c $< -o $@

# A .d names the headers its object includes only after its first build.
$(BUILDDIR)/main.o $(addprefix $(BUILDDIR)/,$(PART_SRCS:.c=.o)): $(TEXT_HDRS) $(IMAGE_HDRS)

$(BUILDDIR)/gen/sintab.c: tools/sintab.py | $(BUILDDIR)
	@mkdir -p $(dir $@)
	python3 tools/sintab.py -o $@

$(BUILDDIR)/gen/sintab.o: $(BUILDDIR)/gen/sintab.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/gen/telefont.o: $(BUILDDIR)/gen/telefont.c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: %.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
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

$(ELF): $(CRT0) $(OBJS) $(LIBC) runtime/demo.ld
	$(LD) $(LDFLAGS) $(CRT0) $(OBJS) $(LIBC) -o $@

# Copy the ELF into STAGE_DIR, for an SD image's FAT32 root.
stage: $(ELF)
	@test -n "$(STAGE_DIR)" || { echo "stage: STAGE_DIR is required" >&2; exit 1; }
	install -d $(STAGE_DIR)
	install -m 0644 $(ELF) $(STAGE_DIR)/

clean:
	rm -rf $(BUILDDIR)
