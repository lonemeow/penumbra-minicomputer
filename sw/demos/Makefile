# Penumbra graphics demos.
#
# Cross-compiles with the in-tree LLVM toolchain against the NetBSD
# sysroot from build.sh distribution.  Each demo is one .c file plus the
# shared terminal layer (dterm.c); the exhibit launcher runs them from
# /usr/local/bin, and they are equally usable from a shell.
#
# perfctr.c is referenced where it lives under benchmark/netbsd-bench/
# rather than copied: pbench uses the same file for its own reporting,
# and one counter implementation serving both is the point.
#
# Output: build/demos/<name> (dynamic) and <name>-static.
#
# Usage:
#   make                   - build everything (both link modes)
#   make dynamic           - dynamic only
#   make static            - static only
#   make overlay           - stage into the rootfs overlay fake-root
#   make clean             - remove build artifacts
#
# Variables:
#   LLVM_PREFIX  path to LLVM build (default: $PROJROOT/build/llvm)
#   DESTDIR      NetBSD sysroot (default: $PROJROOT/build/netbsd-dest)
#   COPT         compiler optimization level (default: -O2)

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

BENCHDIR    := $(PROJROOT)/benchmark/netbsd-bench

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2

CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) \
               -Wall -Wextra -Wno-unused-parameter \
               -I$(dir $(lastword $(MAKEFILE_LIST))) -I$(BENCHDIR)

# dterm.c asks terminfo what the terminal can do.
DEMO_LIBS   := -lterminfo
LDFLAGS_DYN := --target=$(TARGET) --sysroot=$(DESTDIR)
LDFLAGS_STA := --target=$(TARGET) --sysroot=$(DESTDIR) -static

BUILDDIR    := $(PROJROOT)/build/demos
OBJDIR      := $(BUILDDIR)/obj

# ── Demos ─────────────────────────────────────────────────────
# Every demo links the shared objects plus its own single source.
# MATH_DEMOS additionally need libm, for sin() while building a lookup
# table at startup.

DEMOS       := mandelbrot julia plasma lorenz shadebobs penumbra-text
MATH_DEMOS  := plasma shadebobs penumbra-text

SHARED_OBJS := $(OBJDIR)/dterm.o $(OBJDIR)/perfctr.o

.PHONY: all dynamic static overlay clean
all: dynamic static
dynamic: $(addprefix $(BUILDDIR)/,$(DEMOS))
static:  $(addprefix $(BUILDDIR)/,$(addsuffix -static,$(DEMOS)))

# The dynamic binaries go into the rootfs; libc.so is present on-target.
overlay: dynamic
	install -d $(OVERLAY_ROOT)/usr/local/bin
	@for p in $(DEMOS); do \
	    echo "  demos: overlay $$p"; \
	    install -m 0755 $(BUILDDIR)/$$p $(OVERLAY_ROOT)/usr/local/bin/$$p; \
	done

$(BUILDDIR) $(OBJDIR):
	@mkdir -p $@

# Objects are prerequisites of the pattern-matched link rules below,
# which makes them intermediates; without this make deletes them after
# every build and the next one recompiles everything.
.SECONDARY:

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/perfctr.o: $(BENCHDIR)/perfctr.c $(BENCHDIR)/perfctr.h | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# One pair of link rules covers every demo; $(if ...) adds libm only
# where a startup table needs it, so a new demo is a name in DEMOS.
.SECONDEXPANSION:
$(BUILDDIR)/%: $(OBJDIR)/$$*.o $(SHARED_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $< $(SHARED_OBJS) \
	    $(if $(filter $*,$(MATH_DEMOS)),-lm) $(DEMO_LIBS) -o $@

$(BUILDDIR)/%-static: $(OBJDIR)/$$*.o $(SHARED_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $< $(SHARED_OBJS) \
	    $(if $(filter $*,$(MATH_DEMOS)),-lm) $(DEMO_LIBS) -o $@

clean:
	rm -rf $(BUILDDIR)
