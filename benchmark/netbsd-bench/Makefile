# Makefile - pbench: NetBSD-hosted benchmark suite for Penumbra.
#
# Builds:
#   build/netbsd-bench/pbench             - dynamic pbench (libc.so)
#   build/netbsd-bench/pbench-static      - static  pbench (libc.a)
#   build/netbsd-bench/mandelbrot         - dynamic mandelbrot demo
#   build/netbsd-bench/mandelbrot-static  - static  mandelbrot demo
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
LDFLAGS_STA := --target=$(TARGET) --sysroot=$(DESTDIR) -static

BUILDDIR    := $(PROJROOT)/build/netbsd-bench
OBJDIR      := $(BUILDDIR)/obj

# ── Sources ───────────────────────────────────────────────────
# Each benchmark is one .c file under kernel/ or libc/.  Add a new
# benchmark by dropping a .c file in the right directory and listing
# its entry function in pbench.c's registry.

PBENCH_SRCS := pbench.c bench.c \
               kernel/getpid.c kernel/clock_gettime.c \
               kernel/pipe_pingpong.c kernel/fork_exit.c \
               libc/memcpy_sweep.c libc/memset_sweep.c \
               libc/strlen_sweep.c libc/qsort_int.c

PBENCH_OBJS := $(PBENCH_SRCS:%.c=$(OBJDIR)/%.o)

# mandelbrot is a standalone single-file program (not part of pbench's
# registry) — it prints an ASCII Mandelbrot set to stdout, both as an
# ALU benchmark and as a visual demo.
MANDEL_SRCS := mandelbrot.c
MANDEL_OBJS := $(MANDEL_SRCS:%.c=$(OBJDIR)/%.o)

# ── Targets ───────────────────────────────────────────────────

.PHONY: all dynamic static clean

all: dynamic static

dynamic: $(BUILDDIR)/pbench       $(BUILDDIR)/mandelbrot
static:  $(BUILDDIR)/pbench-static $(BUILDDIR)/mandelbrot-static

$(BUILDDIR) $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc:
	@mkdir -p $@

$(OBJDIR)/%.o: %.c bench.h | $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/pbench: $(PBENCH_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(PBENCH_OBJS) -o $@

$(BUILDDIR)/pbench-static: $(PBENCH_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(PBENCH_OBJS) -o $@

$(BUILDDIR)/mandelbrot: $(MANDEL_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(MANDEL_OBJS) -o $@

$(BUILDDIR)/mandelbrot-static: $(MANDEL_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(MANDEL_OBJS) -o $@

clean:
	rm -rf $(BUILDDIR)
