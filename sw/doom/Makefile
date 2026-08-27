# Doom for Penumbra — game-data staging.
#
# The engine source does not live here yet; this Makefile currently
# carries only the `overlay' target, which stages the IWADs dropped in
# wads/ into the shared fake-root that `make image' copies into the
# rootfs.  When the vendored Chocolate Doom tree lands, the engine build
# joins this file and `overlay' grows a dependency on the binary.
#
# Not yet wired into NETBSD_OVERLAYS in the top-level Makefile: staging
# game data into an image that has no engine to read it would be noise.
# Adding it is one line once the engine builds.
#
# Licensing: no WAD is redistributable except Freedoom's, and none is
# committed.  See wads/README.md and entries 4 and 5 of ../../LICENSE.

PROJROOT     := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

WADDIR       := $(dir $(lastword $(MAKEFILE_LIST)))wads

# Where the engine looks for game data on the target.  DOOMWADDIR
# overrides it at runtime; see wads/README.md.
WADDEST      := /usr/local/share/doom

# Every IWAD the developer has dropped in wads/, basename only.  Either
# case of extension is accepted: Freedoom ships lowercase, the Id WADs
# uppercase.
AVAILABLE_WADS := $(sort $(notdir $(wildcard $(WADDIR)/*.wad $(WADDIR)/*.WAD)))

# Stage every IWAD present, so one image can carry Freedoom for the
# exhibit alongside a shareware or retail WAD for testing and the player
# can switch between them with -iwad.  DEFAULT_WAD names the one the
# kiosk launcher starts unattended, which keeps that path deterministic
# whatever else is sitting in wads/.  Nothing reads DEFAULT_WAD until
# that launcher lands; it is the policy the launcher will follow.
#
# The tradeoff: an image built from a tree that happens to hold a
# non-redistributable WAD will carry it.  Check wads/ before building an
# image to hand out.
STAGED_WADS  := $(AVAILABLE_WADS)
DEFAULT_WAD  := freedoom1.wad

.PHONY: overlay
overlay:
	@if [ -z "$(strip $(AVAILABLE_WADS))" ]; then \
	    echo "  doom: no IWAD in $(WADDIR) — Doom will not be runnable"; \
	    echo "  doom: see sw/doom/wads/README.md to obtain one"; \
	else \
	    install -d $(OVERLAY_ROOT)$(WADDEST); \
	    for w in $(STAGED_WADS); do \
	        echo "  doom: overlay $$w"; \
	        install -m 0644 $(WADDIR)/$$w $(OVERLAY_ROOT)$(WADDEST)/$$w; \
	    done; \
	fi
