# Makefile - pbench: NetBSD-hosted benchmark suite for Penumbra.
#
# Builds two ELFs:
#   build/netbsd-bench/pbench         - dynamically linked against libc.so
#   build/netbsd-bench/pbench-static  - statically linked against libc.a
#
# Both run on a Penumbra NetBSD rootfs; mkrootfs.sh -i can overlay them
# into /usr/local/bin/.
#
# Usage:
#   make                   - build both binaries
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

# Note: libc/qsort_int.c is omitted until the GISel G_SCMP legalization
# gap (see doc/TODO.md) is closed — its int comparator can't be compiled
# at -O2 today, and rewriting it to dodge the optimizer would just hide
# the real issue.
SRCS        := pbench.c bench.c \
               kernel/getpid.c kernel/clock_gettime.c \
               kernel/pipe_pingpong.c kernel/fork_exit.c \
               libc/memcpy_sweep.c libc/memset_sweep.c \
               libc/strlen_sweep.c

OBJS        := $(SRCS:%.c=$(OBJDIR)/%.o)

# ── Targets ───────────────────────────────────────────────────

.PHONY: all dynamic static clean

all: dynamic static

dynamic: $(BUILDDIR)/pbench
static:  $(BUILDDIR)/pbench-static

$(BUILDDIR) $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc:
	@mkdir -p $@

$(OBJDIR)/%.o: %.c bench.h | $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/pbench: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(OBJS) -o $@

$(BUILDDIR)/pbench-static: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(OBJS) -o $@

clean:
	rm -rf $(BUILDDIR)
