# Doom game data (WAD files)

Drop an IWAD in this directory and the image build stages it into the
rootfs. Nothing here is tracked by git except this README — see the
"Game data" section of the top-level `.gitignore`.

Doom ships no game data of its own: the engine is one half of the
program and the IWAD is the other. The engine is
[GPL-2.0-or-later](../../../LICENSE); the data is not, and which data
you use decides what you are allowed to hand out.

## Which IWAD

| File | License | Redistributable |
|------|---------|-----------------|
| `freedoom1.wad`, `freedoom2.wad` | BSD 3-Clause | Yes — including on an SD image you give away |
| `DOOM1.WAD` (shareware) | Id Software shareware terms | Only as the complete, unmodified shareware package |
| `DOOM.WAD`, `DOOM2.WAD` (retail) | Proprietary | No |

**Freedoom is the default.** It is the only option that can legally ride
along on a distributed image, so it is what the exhibit build expects.
`sw/doom/fetch-freedoom.sh` downloads it, checks it against the release's
signed SHA256, and unpacks it here:

```sh
sw/doom/fetch-freedoom.sh          # freedoom1.wad, what DEFAULT_WAD names
sw/doom/fetch-freedoom.sh both     # freedoom1.wad and freedoom2.wad
sw/doom/fetch-freedoom.sh freedm   # freedm.wad, smallest download
```

It needs `curl` and `unzip`, and skips anything already downloaded.

The shareware and retail WADs work too — the engine does not care — but
they are for your own machine, not for an image you pass on. Copy the
retail ones in by hand.

## The shareware WAD, for period comparisons

Published 386 and 486 frame rates come from `timedemo demo3` on the
v1.9 shareware WAD, a demo of its E1M7. Freedoom's maps are different
and more detailed — the E1M6 that `BENCH.lmp` plays has 3.2 times the
segs of that E1M7 — so a Freedoom demo does not compare with those
figures.
`sw/doom/fetch-shareware.sh` downloads id's v1.9 shareware package
from the idgames archive, extracts `DOOM1.WAD`, and checks it against
its known SHA256:

```sh
sw/doom/fetch-shareware.sh
doom -iwad DOOM1.WAD -timedemo demo3      # on the target
```

The engine searches for `doom1.wad` before `freedoom1.wad`, so once
`DOOM1.WAD` is staged, a bare `doom` starts the shareware game. Name
the IWAD for anything cut from Freedoom:

```sh
doom -iwad freedoom1.wad -timedemo /usr/local/share/doom/BENCH.lmp
```

Remove `DOOM1.WAD` from this directory before building an image to
hand out.

## Where it ends up

The overlay step installs WADs to `/usr/local/share/doom/` on the
target, which is where the engine looks by default. To point it
somewhere else at runtime, set `DOOMWADDIR` or pass `-iwad`:

```sh
DOOMWADDIR=/mnt/sd doom
doom -iwad /mnt/sd/DOOM1.WAD
```

## Never commit a WAD

`.gitignore` covers `sw/doom/wads/*` and a bare `*.wad` / `*.WAD`
anywhere in the tree, so an accidental `git add` of a stray copy is
caught as well. If you ever need to override that for a genuinely
redistributable file, do it deliberately with `git add -f` — and think
twice, because tens of megabytes of binary data in git history cannot
be removed without a rewrite.
