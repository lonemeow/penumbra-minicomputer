# Makefile - pbench: NetBSD-hosted benchmark suite for Penumbra.
#
# Builds:
#   build/netbsd-bench/pbench             - dynamic pbench (libc.so)
#   build/netbsd-bench/pbench-static      - static  pbench (libc.a)
#   build/netbsd-bench/mandelbrot{,-static} - mandelbrot demo
#   build/netbsd-bench/julia{,-static}      - julia set demo
#   build/netbsd-bench/plasma{,-static}     - plasma effect demo (links -lm)
#   build/netbsd-bench/lorenz{,-static}     - Lorenz attractor demo
#   build/netbsd-bench/shadebobs{,-static}  - shadebobs demo (links -lm)
#   build/netbsd-bench/penumbra-text{,-static} - Penumbra logo screensaver (links -lm)
#
# All run on a Penumbra NetBSD rootfs; mkrootfs.sh -i can overlay them
# into /usr/local/bin/.
#
# Usage:
#   make                   - build everything (both binaries, both link modes)
#   make dynamic           - dynamic only
#   make static            - static only
#   make clean             - remove build artifacts
#
# Variables:
#   LLVM_PREFIX  path to LLVM build (default: $PROJROOT/build/llvm)
#   DESTDIR      NetBSD sysroot (default: $PROJROOT/build/netbsd-dest)
#   COPT         compiler optimization level (default: -O2)

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX ?= $(PROJROOT)/build/llvm
DESTDIR     ?= $(PROJROOT)/build/netbsd-dest

CC          := $(LLVM_PREFIX)/bin/clang
TARGET      := penumbra-unknown-netbsd
COPT        ?= -O2

# Sysroot exposes <stdio.h>, <time.h>, libc.{a,so}, crt{i,n,0}.o from
# build.sh distribution.  No -ffreestanding here — this is hosted code.
CFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) \
               -Wall -Wextra -Wno-unused-parameter \
               -I$(dir $(lastword $(MAKEFILE_LIST)))

LDFLAGS_DYN := --target=$(TARGET) --sysroot=$(DESTDIR)
# dterm.c asks terminfo what the terminal can do.
DEMO_LIBS   := -lterminfo
LDFLAGS_STA := --target=$(TARGET) --sysroot=$(DESTDIR) -static

BUILDDIR    := $(PROJROOT)/build/netbsd-bench
OBJDIR      := $(BUILDDIR)/obj
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

# ── Sources ───────────────────────────────────────────────────
# Each benchmark is one .c file under kernel/ or libc/.  Add a new
# benchmark by dropping a .c file in the right directory and listing
# its entry function in pbench.c's registry.

PBENCH_SRCS := pbench.c bench.c perfctr.c \
               kernel/getpid.c kernel/clock_gettime.c \
               kernel/pipe_pingpong.c kernel/fork_exit.c kernel/fork_exec.c \
               libc/memcpy_sweep.c libc/memset_sweep.c \
               libc/strlen_sweep.c libc/qsort_int.c

PBENCH_OBJS := $(PBENCH_SRCS:%.c=$(OBJDIR)/%.o)

# mandelbrot and julia are standalone single-file demo programs (not
# part of pbench's registry) — they print fixed-point fractals to
# stdout, doubling as ALU benchmarks and as visual demos.
MANDEL_SRCS := mandelbrot.c
MANDEL_OBJS := $(MANDEL_SRCS:%.c=$(OBJDIR)/%.o) $(OBJDIR)/perfctr.o $(OBJDIR)/dterm.o

JULIA_SRCS  := julia.c
JULIA_OBJS  := $(JULIA_SRCS:%.c=$(OBJDIR)/%.o) $(OBJDIR)/perfctr.o $(OBJDIR)/dterm.o

PLASMA_SRCS := plasma.c
PLASMA_OBJS := $(PLASMA_SRCS:%.c=$(OBJDIR)/%.o) $(OBJDIR)/perfctr.o $(OBJDIR)/dterm.o

LORENZ_SRCS := lorenz.c
LORENZ_OBJS := $(LORENZ_SRCS:%.c=$(OBJDIR)/%.o) $(OBJDIR)/perfctr.o $(OBJDIR)/dterm.o

SHADEBOBS_SRCS := shadebobs.c
SHADEBOBS_OBJS := $(SHADEBOBS_SRCS:%.c=$(OBJDIR)/%.o) $(OBJDIR)/perfctr.o $(OBJDIR)/dterm.o

PTEXT_SRCS := penumbra-text.c
PTEXT_OBJS := $(PTEXT_SRCS:%.c=$(OBJDIR)/%.o) $(OBJDIR)/perfctr.o $(OBJDIR)/dterm.o

# ── Targets ───────────────────────────────────────────────────

.PHONY: all dynamic static overlay clean

all: dynamic static

# Install the dynamic binaries into the shared overlay fake-root so the
# rootfs image (mkrootfs -O) absorbs them.  libc.so is present on-target.
OVERLAY_PROGS := pbench mandelbrot julia plasma lorenz shadebobs penumbra-text
overlay: dynamic
	install -d $(OVERLAY_ROOT)/usr/local/bin
	@for p in $(OVERLAY_PROGS); do \
	    echo "  benchmark: overlay $$p"; \
	    install -m 0755 $(BUILDDIR)/$$p $(OVERLAY_ROOT)/usr/local/bin/$$p; \
	done

dynamic: $(BUILDDIR)/pbench        $(BUILDDIR)/mandelbrot        $(BUILDDIR)/julia        $(BUILDDIR)/plasma        $(BUILDDIR)/lorenz        $(BUILDDIR)/shadebobs        $(BUILDDIR)/penumbra-text
static:  $(BUILDDIR)/pbench-static $(BUILDDIR)/mandelbrot-static $(BUILDDIR)/julia-static $(BUILDDIR)/plasma-static $(BUILDDIR)/lorenz-static $(BUILDDIR)/shadebobs-static $(BUILDDIR)/penumbra-text-static

$(BUILDDIR) $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc:
	@mkdir -p $@

$(OBJDIR)/%.o: %.c bench.h | $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/pbench: $(PBENCH_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(PBENCH_OBJS) -o $@

$(BUILDDIR)/pbench-static: $(PBENCH_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(PBENCH_OBJS) -o $@

$(BUILDDIR)/mandelbrot: $(MANDEL_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(MANDEL_OBJS) $(DEMO_LIBS) -o $@

$(BUILDDIR)/mandelbrot-static: $(MANDEL_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(MANDEL_OBJS) $(DEMO_LIBS) -o $@

$(BUILDDIR)/julia: $(JULIA_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(JULIA_OBJS) $(DEMO_LIBS) -o $@

$(BUILDDIR)/julia-static: $(JULIA_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(JULIA_OBJS) $(DEMO_LIBS) -o $@

# plasma needs libm (sin() at startup to build the lookup table).
$(BUILDDIR)/plasma: $(PLASMA_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(PLASMA_OBJS) -lm $(DEMO_LIBS) -o $@

$(BUILDDIR)/plasma-static: $(PLASMA_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(PLASMA_OBJS) -lm $(DEMO_LIBS) -o $@

$(BUILDDIR)/lorenz: $(LORENZ_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(LORENZ_OBJS) $(DEMO_LIBS) -o $@

$(BUILDDIR)/lorenz-static: $(LORENZ_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(LORENZ_OBJS) $(DEMO_LIBS) -o $@

# shadebobs needs libm (sin() at startup to build the lookup table).
$(BUILDDIR)/shadebobs: $(SHADEBOBS_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(SHADEBOBS_OBJS) -lm $(DEMO_LIBS) -o $@

$(BUILDDIR)/shadebobs-static: $(SHADEBOBS_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(SHADEBOBS_OBJS) -lm $(DEMO_LIBS) -o $@

# penumbra-text needs libm (sin() at startup to build the lookup table
# used by the 1D plasma underline).
$(BUILDDIR)/penumbra-text: $(PTEXT_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(PTEXT_OBJS) -lm $(DEMO_LIBS) -o $@

$(BUILDDIR)/penumbra-text-static: $(PTEXT_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(PTEXT_OBJS) -lm $(DEMO_LIBS) -o $@

clean:
	rm -rf $(BUILDDIR)
