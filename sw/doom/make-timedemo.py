#!/usr/bin/env python3
"""Cut a fixed-length demo out of an IWAD for benchmarking.

A full demo runs thousands of tics, which at this machine's frame rate is
hours.  Timing a fixed prefix instead makes runs directly comparable: the
same frames, the same level load, so a difference between two runs is the
thing being measured rather than how long someone waited before pressing
Ctrl-C.

    ./make-timedemo.py wads/freedoom1.wad 200 wads/BENCH.lmp

Doom's -timedemo takes a file before it takes a lump name, so the result
is played with `-timedemo /usr/local/share/doom/BENCH.lmp'.
"""

import struct
import sys

DEMO_MARKER = 0x80
MAXPLAYERS = 4


def read_lump(wad, name):
    with open(wad, "rb") as f:
        magic, count, diroff = struct.unpack("<4sii", f.read(12))
        if magic not in (b"IWAD", b"PWAD"):
            sys.exit(f"{wad}: not a WAD")
        f.seek(diroff)
        entries = [struct.unpack("<ii8s", f.read(16)) for _ in range(count)]
        for offset, size, raw in entries:
            if raw.rstrip(b"\0").decode("ascii", "replace").upper() == name:
                f.seek(offset)
                return f.read(size)
    sys.exit(f"{wad}: no lump named {name}")


def truncate(demo, tics):
    version = demo[0]
    if version <= 4:
        sys.exit(f"demo version {version} predates the header this reads")

    # version, skill, episode, map, deathmatch, respawn, fast, nomonsters,
    # consoleplayer, then one playeringame byte per player.
    header = 9 + MAXPLAYERS
    players = sum(1 for b in demo[9:header] if b)
    if players == 0:
        sys.exit("demo has no active players")

    stride = 4 * players
    available = (len(demo) - header - 1) // stride
    if tics > available:
        sys.exit(f"demo holds {available} tics, {tics} requested")

    return demo[:header] + demo[header:header + tics * stride] \
        + bytes([DEMO_MARKER])


def main():
    if len(sys.argv) not in (4, 5):
        sys.exit(f"usage: {sys.argv[0]} <iwad> <tics> <out.lmp> [lumpname]")

    wad, tics, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    name = (sys.argv[4] if len(sys.argv) == 5 else "DEMO1").upper()

    short = truncate(read_lump(wad, name), tics)
    with open(out, "wb") as f:
        f.write(short)

    print(f"{out}: {tics} tics, {len(short)} bytes (from {name} in {wad})")


if __name__ == "__main__":
    main()
