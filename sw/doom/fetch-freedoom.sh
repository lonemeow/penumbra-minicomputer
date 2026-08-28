#!/bin/sh
#
# Fetch a Freedoom IWAD into wads/.
#
# Freedoom is BSD 3-Clause: unlike the Id WADs it can be redistributed,
# including on an image handed out at a show.  See wads/README.md.
#
#   ./fetch-freedoom.sh          freedoom1.wad — what DEFAULT_WAD names
#   ./fetch-freedoom.sh both     freedoom1.wad and freedoom2.wad
#   ./fetch-freedoom.sh freedm   freedm.wad — smallest download

set -eu

VERSION=0.13.0
BASE="https://github.com/freedoom/freedoom/releases/download/v${VERSION}"

# From the release's signed CHECKSUM file.
SHA_FREEDOOM=3f9b264f3e3ce503b4fb7f6bdcb1f419d93c7b546f4df3e874dd878db9688f59
SHA_FREEDM=b420f13508ef745d7b38e83d15e55e0fc0b09d9a503c96741cddd9773d43f7c9

here=$(cd "$(dirname "$0")" && pwd)
waddir="$here/wads"

case "${1:-freedoom1}" in
    freedoom1) archive="freedoom-${VERSION}.zip"; sha=$SHA_FREEDOOM
               wads="freedoom1.wad" ;;
    both)      archive="freedoom-${VERSION}.zip"; sha=$SHA_FREEDOOM
               wads="freedoom1.wad freedoom2.wad" ;;
    freedm)    archive="freedm-${VERSION}.zip";   sha=$SHA_FREEDM
               wads="freedm.wad" ;;
    *) echo "usage: $0 [freedoom1|both|freedm]" >&2; exit 2 ;;
esac

for t in sha256sum "shasum -a 256" "cksum -a sha256"; do
    if command -v "${t%% *}" >/dev/null 2>&1; then sha256="$t"; break; fi
done
: "${sha256:?no sha256 tool found}"
command -v unzip >/dev/null || { echo "unzip not found" >&2; exit 1; }

missing=""
for w in $wads; do
    [ -f "$waddir/$w" ] || missing="$missing $w"
done
if [ -z "$missing" ]; then
    echo "already present:$(for w in $wads; do printf ' %s' "$w"; done)"
    exit 0
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "fetching $archive"
curl -fL --progress-bar -o "$tmp/$archive" "$BASE/$archive"

# The signed checksum is the only thing standing between a truncated or
# tampered download and an IWAD staged into an image.
echo "verifying"
got=$($sha256 "$tmp/$archive" | tr ' ' '\n' | grep -Ex '[0-9a-f]{64}' | head -1)
if [ "$got" != "$sha" ]; then
    echo "checksum mismatch for $archive" >&2
    echo "  expected $sha" >&2
    echo "  got      $got" >&2
    exit 1
fi

# Match on the trailing name: the archive nests the WADs under a
# version-stamped directory.
mkdir -p "$waddir"
for w in $wads; do
    unzip -j -o -q "$tmp/$archive" "*/$w" -d "$waddir"
    echo "  $waddir/$w"
done
