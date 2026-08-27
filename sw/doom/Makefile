# Doom for Penumbra: cross-compiles the vendored Chocolate Doom tree
# against the NetBSD sysroot and stages the binary plus any IWAD in
# wads/ into the fake-root `make image' copies into the rootfs.
#
# SDL is replaced only where it talks to hardware; sdl_compat.h and
# upstream's DISABLE_SDL2MIXER / DISABLE_SDL2NET cover the rest.
#
# The engine is GPL-2.0-or-later and no WAD is committed — see
# wads/README.md and entries 4 and 5 of ../../LICENSE.

PROJROOT     := $(realpath $(dir $(lastword $(MAKEFILE_LIST)))/../..)
LLVM_PREFIX  ?= $(PROJROOT)/build/llvm
DESTDIR      ?= $(PROJROOT)/build/netbsd-dest
OVERLAY_ROOT ?= $(PROJROOT)/build/netbsd-overlay

HERE         := $(dir $(lastword $(MAKEFILE_LIST)))
CD           := $(HERE)chocolate-doom
WADDIR       := $(HERE)wads

CC           := $(LLVM_PREFIX)/bin/clang
TARGET       := penumbra-unknown-netbsd
COPT         ?= -O2

BUILDDIR     := $(PROJROOT)/build/doom
OBJDIR       := $(BUILDDIR)/obj

# Where the engine looks for game data; DOOMWADDIR overrides at runtime.
WADDEST      := /usr/local/share/doom

# ── Sources ──────────────────────────────────────────────────────

# Not one file in src/doom/ touches SDL, so the game builds whole.
DOOM_SRCS    := $(wildcard $(CD)/src/doom/*.c)

# The common layer minus what cannot compile without real SDL. Excluding
# rather than listing keeps new upstream files in the build by default.
#
#   replaced below by a NetBSD implementation
COMMON_EXCL  := i_video.c i_input.c i_joystick.c i_timer.c i_main.c
#   audio backends; each opens a real SDL device
COMMON_EXCL  += i_sdlsound.c i_sdlmusic.c i_musicpack.c i_pcsound.c \
                i_flmusic.c i_cdmus.c
#   SDL hi-res startup screen, used by Strife rather than Doom
COMMON_EXCL  += i_videohr.c
#   Windows-only
COMMON_EXCL  += i_winmusic.c w_file_win32.c
#   the dedicated server's own main()
COMMON_EXCL  += d_dedicated.c

COMMON_SRCS  := $(filter-out $(addprefix $(CD)/src/,$(COMMON_EXCL)), \
                             $(wildcard $(CD)/src/*.c))

# Named for the interface each targets: the framebuffer and keyboard
# paths are plain wscons, nothing Penumbra-specific.
PORT_SRCS    := $(HERE)i_video_netbsd.c \
                $(HERE)i_input_netbsd.c \
                $(HERE)i_timer_netbsd.c \
                $(HERE)i_joystick_null.c \
                $(HERE)i_main_netbsd.c

SRCS         := $(DOOM_SRCS) $(COMMON_SRCS) $(PORT_SRCS)
OBJS         := $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(SRCS)))

# ── Flags ────────────────────────────────────────────────────────

# config.h and sdl_compat.h come from here; sdl-compat/ answers
# upstream's `#include "SDL*.h"' lines.
INCLUDES     := -I$(HERE) -I$(HERE)sdl-compat -I$(CD)/src -I$(CD)/src/doom \
                -I$(CD)/textscreen -I$(CD)/opl -I$(CD)/pcsound

# net_sdl.c still defines net_sdl_module under DISABLE_SDL2NET, so the
# multiplayer code links — d_loop.c reaches the net layer single-player.
DEFINES      := -DDISABLE_SDL2MIXER=1 -DDISABLE_SDL2NET=1

CFLAGS       := --target=$(TARGET) --sysroot=$(DESTDIR) $(COPT) \
                -std=c99 -Wall -Wdeclaration-after-statement \
                -Wno-unused-parameter $(INCLUDES) $(DEFINES)
LDFLAGS      := --target=$(TARGET) --sysroot=$(DESTDIR)
LDLIBS       := -lm

.PHONY: all clean overlay
all: $(BUILDDIR)/doom

$(BUILDDIR)/doom: $(OBJS) | $(BUILDDIR)
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

# No basename collisions across the three source directories.
vpath %.c $(CD)/src $(CD)/src/doom $(HERE)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR) $(OBJDIR):
	@mkdir -p $@


# ── Game data ────────────────────────────────────────────────────

# Either extension case: Freedoom ships lowercase, the Id WADs uppercase.
AVAILABLE_WADS := $(sort $(notdir $(wildcard $(WADDIR)/*.wad $(WADDIR)/*.WAD)))

# Stage every IWAD present, so one image carries Freedoom for the exhibit
# alongside a test WAD and the player switches with -iwad. DEFAULT_WAD is
# the one the kiosk launcher starts unattended.
#
# Tradeoff: an image built from a tree holding a non-redistributable WAD
# will carry it. Check wads/ before building one to hand out.
STAGED_WADS  := $(AVAILABLE_WADS)
DEFAULT_WAD  := freedoom1.wad

overlay: all
	install -d $(OVERLAY_ROOT)/usr/local/bin
	install -m 0755 $(BUILDDIR)/doom $(OVERLAY_ROOT)/usr/local/bin/doom
	@echo "  doom: overlay doom"
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

clean:
	rm -rf $(BUILDDIR)
