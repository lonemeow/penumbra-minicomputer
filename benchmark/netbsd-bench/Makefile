# Makefile - pbench: NetBSD-hosted benchmark suite for Penumbra.
#
# Builds:
#   build/netbsd-bench/pbench             - dynamic pbench (libc.so)
#   build/netbsd-bench/pbench-static      - static  pbench (libc.a)
#
# Runs on a Penumbra NetBSD rootfs; the overlay step below stages it
# into /usr/local/bin/.  The graphics demos live in sw/demos/ and build
# separately; they share perfctr.c from here, which they reference in
# place rather than copying.
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

# ── Targets ───────────────────────────────────────────────────

.PHONY: all dynamic static overlay clean

all: dynamic static

# Install the dynamic binaries into the shared overlay fake-root so the
# rootfs image absorbs them.  libc.so is present on-target.
overlay: dynamic
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/pbench $(OVERLAY_ROOT)/usr/local/bin/pbench

dynamic: $(BUILDDIR)/pbench
static:  $(BUILDDIR)/pbench-static

$(BUILDDIR) $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc:
	@mkdir -p $@

$(OBJDIR)/%.o: %.c bench.h | $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/pbench: $(PBENCH_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(PBENCH_OBJS) -o $@

$(BUILDDIR)/pbench-static: $(PBENCH_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(PBENCH_OBJS) -o $@

clean:
	rm -rf $(BUILDDIR)
