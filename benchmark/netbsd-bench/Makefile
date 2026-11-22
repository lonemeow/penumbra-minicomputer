# Makefile - pbench: NetBSD-hosted benchmark suite for Penumbra.
#
# Builds:
#   build/netbsd-bench/pbench                 - dynamic pbench (libc.so)
#   build/netbsd-bench/pbench-static          - static  pbench (libc.a)
#   build/netbsd-bench/pbench-pthread         - dynamic, linked with libpthread
#   build/netbsd-bench/pbench-pthread-static  - static,  linked with libpthread
#
# pbench-pthread is pbench's own objects plus the pthread category.  It is
# a binary of its own because libpthread, once linked, makes the whole
# process threaded before main() runs; a case run in both binaries prices
# what that costs it.
#
# Runs on a Penumbra NetBSD rootfs; the overlay step below stages it
# into /usr/local/bin/.  The graphics demos live in sw/demos/ and build
# separately; they share perfctr.c from here, which they reference in
# place rather than copying.
#
# Usage:
#   make                   - build everything (all binaries, both link modes)
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
LDLIBS         := -lm
LDLIBS_PTHREAD := -lpthread -lm

BUILDDIR    := $(PROJROOT)/build/netbsd-bench
OBJDIR      := $(BUILDDIR)/obj
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

# ── Sources ───────────────────────────────────────────────────
# Each benchmark is one .c file under kernel/, libc/, audio/ or pthread/.
# Add a new benchmark by dropping a .c file in the right directory and
# listing its entry function in that category's registry (registry.c,
# or pthread/registry.c for cases that need libpthread).

# Compiled once and linked into every binary.
PBENCH_SRCS  := pbench.c bench.c perfctr.c registry.c \
                kernel/getpid.c kernel/clock_gettime.c \
                kernel/pipe_pingpong.c kernel/fork_exit.c kernel/fork_exec.c \
                libc/memcpy_sweep.c libc/memset_sweep.c \
                libc/strlen_sweep.c libc/qsort_int.c \
                audio/mix.c

# What each binary links on top: the suite naming its registries, and for
# pbench-pthread the pthread category.
SUITE_SRCS   := suite.c
PTHREAD_SRCS := suite_pthread.c pthread/registry.c pthread/partner.c \
                pthread/pipe_pingpong.c pthread/cond_pingpong.c pthread/mutex.c

PBENCH_OBJS  := $(PBENCH_SRCS:%.c=$(OBJDIR)/%.o)
SUITE_OBJS   := $(SUITE_SRCS:%.c=$(OBJDIR)/%.o)
PTHREAD_OBJS := $(PTHREAD_SRCS:%.c=$(OBJDIR)/%.o)

# ── Targets ───────────────────────────────────────────────────

.PHONY: all dynamic static overlay clean

all: dynamic static

# Install the dynamic binaries into the shared overlay fake-root so the
# rootfs image absorbs them.  libc.so is present on-target.
overlay: dynamic
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/pbench $(OVERLAY_ROOT)/usr/local/bin/pbench
	install -m 0755 $(BUILDDIR)/pbench-pthread \
	    $(OVERLAY_ROOT)/usr/local/bin/pbench-pthread

dynamic: $(BUILDDIR)/pbench $(BUILDDIR)/pbench-pthread
static:  $(BUILDDIR)/pbench-static $(BUILDDIR)/pbench-pthread-static

$(BUILDDIR) $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc $(OBJDIR)/audio \
    $(OBJDIR)/pthread:
	@mkdir -p $@

$(OBJDIR)/%.o: %.c bench.h | $(OBJDIR) $(OBJDIR)/kernel $(OBJDIR)/libc \
    $(OBJDIR)/audio $(OBJDIR)/pthread
	$(CC) $(CFLAGS) -c $< -o $@

$(PTHREAD_OBJS): pthread/partner.h

$(BUILDDIR)/pbench: $(PBENCH_OBJS) $(SUITE_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(PBENCH_OBJS) $(SUITE_OBJS) $(LDLIBS) -o $@

$(BUILDDIR)/pbench-static: $(PBENCH_OBJS) $(SUITE_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(PBENCH_OBJS) $(SUITE_OBJS) $(LDLIBS) -o $@

$(BUILDDIR)/pbench-pthread: $(PBENCH_OBJS) $(PTHREAD_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_DYN) $(PBENCH_OBJS) $(PTHREAD_OBJS) $(LDLIBS_PTHREAD) -o $@

$(BUILDDIR)/pbench-pthread-static: $(PBENCH_OBJS) $(PTHREAD_OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS_STA) $(PBENCH_OBJS) $(PTHREAD_OBJS) $(LDLIBS_PTHREAD) -o $@

clean:
	rm -rf $(BUILDDIR)
