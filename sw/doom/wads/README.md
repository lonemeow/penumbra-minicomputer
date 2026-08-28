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
they are for your own machine, not for an image you pass on. Copy those
in by hand.

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
