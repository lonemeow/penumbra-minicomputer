# Minimal /sbin/init for Penumbra
#
# Produces a static ELF binary suitable for the kernel's execve().
# Usage:
#   make                        # build using LLVM from build/llvm/
#   make LLVM_PREFIX=/path/to   # override toolchain location
#   make install DESTDIR=...    # copy to DESTDIR/sbin/init

LLVM_PREFIX ?= $(CURDIR)/../../build/llvm
CC      := $(LLVM_PREFIX)/bin/clang
LD      := $(LLVM_PREFIX)/bin/ld.lld
OBJCOPY := $(LLVM_PREFIX)/bin/llvm-objcopy

TARGET  := penumbra-unknown-none
CFLAGS  := --target=$(TARGET) -ffreestanding -nostdlib -O2 -Wall
LDFLAGS := -nostdlib -T $(CURDIR)/init.ld

BUILD   := ../../build/init
OUT     := $(BUILD)/init

.PHONY: all clean install

all: $(OUT)

$(BUILD):
	mkdir -p $@

$(BUILD)/init.o: init.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(OUT): $(BUILD)/init.o
	$(LD) $(LDFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD)

install: $(OUT)
	install -d $(DESTDIR)/sbin
	install -m 755 $(OUT) $(DESTDIR)/sbin/init
