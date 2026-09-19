# Skrolli Party 2026 — Wild compo entry

A bare-metal demo for the Penumbra minicomputer, captured to video and
submitted remotely to the Wild compo at Skrolli Party 2026.

Bare metal rather than under NetBSD: the demo owns the machine, so
frame timing depends on nothing but the code, and every cycle of a
37.5 MHz budget is available to the effects.

## Layout

```
runtime/     this production's framework — startup, MMU and cache
             bring-up, device access, the frame loop and the precalc
             scheduler.  Deliberately not shared: a later production
             may target different hardware and want different
             assumptions.
parts/       one file per effect, and its typed text as <name>.txt.
music/       song source and its samples.
assets/      images as indexed PNGs, converted at build time.
tools/       asset converters, run on the host at build time.
capture/     recording and encode scripts.
```

The MOD replayer is `runtime/modplay.c`, a ProTracker subset driving
one `CLASS_AUDIO` channel per track. `tools/mod2song.py` converts the
module at build time and lists what the subset drops.

## Documents

- [CLAUDE.md](CLAUDE.md) — ground rules on what AI assistance may and
  may not do here, and why.
- [NOTES.md](NOTES.md) — measured hardware costs, the frame budget,
  and the constraints every part is designed against.
- [CREDITS.md](CREDITS.md) — provenance ledger; the entry's info file
  is generated from it.

## Building and running

A PIE ELF loaded from the card's FAT32 root, like the bare-metal
benchmarks. `make image` and `make sdimage-bench` both put it there,
passing `MOD=` through. Parts are cued to the song, so a build without
one links but panics at start.

```sh
make -C sw/demoscene/skrolli2026 MOD=path/to/song.mod
# on the target's ROM monitor:
boot sd:0,0/SKR2026.ELF
```

Assets link into the ELF as `.rodata` rather than loading separately —
a couple of hundred KB against 32 MB of RAM, in exchange for one file,
no SD driver in the demo, and a deterministic load.

## Capture

Video comes off the GPDI output through a USB3 capture device at
640x480; audio comes off the 3.5 mm jack separately, since the output
is DVI signalling with no audio islands. The two are aligned in post
against the demo's own beat-synced flashes.

Final delivery is 1080p60: the 640x480 capture scales 2x
nearest-neighbour to 1280x960 and is pillarboxed, so pixels stay
crisp.
