#!/bin/sh
#
# Fetch the Doom v1.9 shareware IWAD into wads/.
#
# id's last shareware release, from the idgames archive.  Its DEMO3 is
# the demo period benchmarks ran as `timedemo demo3`, so this is the
# IWAD to use when comparing against published 386 and 486 frame rates:
#
#   doom -iwad DOOM1.WAD -timedemo demo3
#
# The shareware terms allow redistribution only as the complete,
# unmodified package.  DOOM1.WAD on its own must not ride along on an
# image you hand to anyone -- see wads/README.md.

set -eu

# idgames mirrors, tried in order; each carries the same doom19s.zip.
MIRRORS="https://www.gamers.org/pub/idgames
https://ftpmirror1.infania.net/pub/idgames
https://youfailit.net/pub/idgames"
PACKAGE=idstuff/doom/doom19s.zip

# DOOM1.WAD v1.9: MD5 f0cefca49926d00903cf57551d901abe, the hash source
# ports' IWAD tables identify it by.  The WAD is checked rather than the
# zip because the WAD is what gets staged into an image.
SHA_WAD=1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771

here=$(cd "$(dirname "$0")" && pwd)
waddir="$here/wads"

for t in sha256sum "shasum -a 256" "cksum -a sha256"; do
    if command -v "${t%% *}" >/dev/null 2>&1; then sha256="$t"; break; fi
done
: "${sha256:?no sha256 tool found}"
command -v unzip >/dev/null || { echo "unzip not found" >&2; exit 1; }

# The engine and the image build both accept either case.
for w in DOOM1.WAD doom1.wad; do
    if [ -f "$waddir/$w" ]; then
        echo "already present: $w"
        exit 0
    fi
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

sha_of() {
    $sha256 "$1" | tr ' ' '\n' | grep -Ex '[0-9a-f]{64}' | head -1
}

# The zip holds id's installer: DOOMS_19.1 and DOOMS_19.2 are two halves
# of one self-extracting ZIP, which unzip reads once they are joined
# (it warns about the DOS stub in front, hence the ignored status).
extract_wad() {
    rm -rf "$tmp/pkg" && mkdir "$tmp/pkg"
    unzip -q -o "$tmp/doom19s.zip" 'DOOMS_19.*' -d "$tmp/pkg"
    cat "$tmp/pkg/DOOMS_19.1" "$tmp/pkg/DOOMS_19.2" > "$tmp/pkg/dooms19.exe"
    unzip -q -o "$tmp/pkg/dooms19.exe" DOOM1.WAD -d "$tmp" 2>/dev/null || true
    [ -f "$tmp/DOOM1.WAD" ]
}

fetched=false
for m in $MIRRORS; do
    echo "fetching $m/$PACKAGE"
    if curl -fL --progress-bar -o "$tmp/doom19s.zip" "$m/$PACKAGE"; then
        fetched=true
        break
    fi
done
if ! $fetched; then
    echo "no mirror served $PACKAGE" >&2
    exit 1
fi

extract_wad || { echo "no DOOM1.WAD in $PACKAGE" >&2; exit 1; }

# A mismatch stops the run rather than moving on to the next mirror: an
# unreachable mirror is noise, but a mirror serving a different WAD is
# something to look at, not route around.
got=$(sha_of "$tmp/DOOM1.WAD")
if [ "$got" != "$SHA_WAD" ]; then
    echo "checksum mismatch for DOOM1.WAD" >&2
    echo "  expected $SHA_WAD" >&2
    echo "  got      $got" >&2
    exit 1
fi

mkdir -p "$waddir"
mv "$tmp/DOOM1.WAD" "$waddir/DOOM1.WAD"
echo "  $waddir/DOOM1.WAD"

# The engine's IWAD search tries doom1.wad before freedoom1.wad, so from
# here on a bare `doom` starts the shareware game -- and BENCH.lmp, cut
# from Freedoom, desyncs unless the IWAD is named.
cat <<EOF
note: Doom now starts DOOM1.WAD when no -iwad is given.  Name the IWAD
      for Freedoom demos:
        doom -iwad freedoom1.wad -timedemo /usr/local/share/doom/BENCH.lmp
      and remove DOOM1.WAD from wads/ before building an image to hand out.
EOF
