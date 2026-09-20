# sw/common — pieces more than one userland tool builds into itself
#
# There is no library here: each tool compiles the files it needs, so
# the tool's own flags — target, sysroot, optimization — decide how a
# shared file is built.  A tool picks them up with -I$(PROJROOT)/sw/common
# and by naming the source in its own object list.
#
# Output: build/common/screen_test, build/common/fixed_test (host)

PROJROOT    := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
BUILDDIR    := $(PROJROOT)/build/common

HOSTCC      ?= cc
WARN        := -Wall -Wextra -Wno-unused-parameter

.PHONY: test clean

# The screen layer's damage-diff and the fixed-point helpers, on the
# host: no terminal involved in either.
test: $(BUILDDIR)/screen_test $(BUILDDIR)/fixed_test
	$(BUILDDIR)/screen_test
	@echo
	$(BUILDDIR)/fixed_test

$(BUILDDIR)/screen_test: screen_test.c screen.c screen.h
	install -d $(BUILDDIR)
	$(HOSTCC) $(WARN) -I. screen_test.c screen.c -o $@

$(BUILDDIR)/fixed_test: fixed_test.c fixed.h
	install -d $(BUILDDIR)
	$(HOSTCC) $(WARN) -I. fixed_test.c -o $@

clean:
	rm -rf $(BUILDDIR)
