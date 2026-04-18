#!/bin/bash
# mkrootfs.sh — Create an FFS root filesystem image for Penumbra.
#
# Populates a staging directory from DESTDIR (build.sh distribution output)
# and creates an FFS image using nbmakefs with METALOG for correct
# file permissions and ownership.
#
# Usage:
#   mkrootfs.sh -d DESTDIR -o rootfs.img [options]
#
# Options:
#   -d DESTDIR      Source directory (build/netbsd-dest, required)
#   -o FILE         Output FFS image (required)
#   -s SIZE_MB      Image size in MB (default: auto-fit with 20% slack)
#   -m              Minimal mode: rescue + etc + lib only (no /usr)
#   -T TOOLDIR      NetBSD tools directory (default: auto-detect)
#   -v              Verbose output
#
# The image is a raw FFS filesystem suitable for dd'ing into a partition.
# Minimal mode is useful for initial bringup without ld.elf_so — the
# rescue binary is statically linked and provides ~150 commands.
#
# METALOG from DESTDIR is used for file permissions/ownership.
# Essential device nodes (console, null, zero, tty) are added via
# an inline mtree spec — MAKEDEV runs at first boot for the rest.

set -euo pipefail

# --- Defaults ---------------------------------------------------------------

DESTDIR=""
OUTPUT=""
SIZE_MB=""
MINIMAL=0
TOOLDIR=""
VERBOSE=0

# --- Parse arguments --------------------------------------------------------

usage() {
    sed -n '2,/^$/{ s/^# \?//; p; }' "$0"
    exit 1
}

while getopts "d:o:s:mT:vh" opt; do
    case $opt in
        d) DESTDIR="$OPTARG" ;;
        o) OUTPUT="$OPTARG" ;;
        s) SIZE_MB="$OPTARG" ;;
        m) MINIMAL=1 ;;
        T) TOOLDIR="$OPTARG" ;;
        v) VERBOSE=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ -z "$DESTDIR" ] || [ -z "$OUTPUT" ]; then
    echo "Error: -d DESTDIR and -o FILE are required" >&2
    usage
fi

if [ ! -d "$DESTDIR" ]; then
    echo "Error: DESTDIR does not exist: $DESTDIR" >&2
    exit 1
fi

# --- Locate NetBSD tools ----------------------------------------------------

find_tooldir() {
    local script_dir
    script_dir="$(cd "$(dirname "$0")" && pwd)"
    local dir="$script_dir"
    while [ "$dir" != "/" ]; do
        if [ -d "$dir/build/netbsd-tools/bin" ]; then
            echo "$dir/build/netbsd-tools/bin"
            return 0
        fi
        dir="$(dirname "$dir")"
    done
    return 1
}

if [ -z "$TOOLDIR" ]; then
    TOOLDIR="$(find_tooldir)" || {
        echo "Error: cannot find NetBSD tools. Build them first or use -T." >&2
        exit 1
    }
fi

NBMAKEFS="$TOOLDIR/nbmakefs"
if [ ! -x "$NBMAKEFS" ]; then
    echo "Error: nbmakefs not found in $TOOLDIR" >&2
    exit 1
fi

log() { [ "$VERBOSE" -eq 1 ] && echo "  $*" >&2 || true; }

# --- Populate staging directory ---------------------------------------------

TMPDIR=$(mktemp -d)
STAGING="$TMPDIR/root"
mkdir -p "$STAGING"

cleanup() { rm -rf "$TMPDIR"; }
trap cleanup EXIT

if [ "$MINIMAL" -eq 1 ]; then
    log "Minimal mode: rescue + etc + lib"

    # Core directories
    mkdir -p "$STAGING"/{dev,etc,tmp,var/run,var/tmp,mnt,root}
    mkdir -p "$STAGING"/{bin,sbin}

    # Rescue binary — statically linked, ~150 commands
    if [ -d "$DESTDIR/rescue" ]; then
        cp -a "$DESTDIR/rescue" "$STAGING/rescue"
    else
        echo "Error: $DESTDIR/rescue not found" >&2
        exit 1
    fi

    # /sbin/init and /bin/sh must exist.  Point them at rescue.
    ln -s /rescue/init "$STAGING/sbin/init"
    ln -s /rescue/sh "$STAGING/bin/sh"

    # Shared libraries — needed if any non-rescue binary is run
    for d in lib libexec; do
        if [ -d "$DESTDIR/$d" ]; then
            cp -a "$DESTDIR/$d" "$STAGING/$d"
        fi
    done

    # Minimal /etc from distribution
    if [ -d "$DESTDIR/etc" ]; then
        cp -a "$DESTDIR/etc" "$STAGING/etc"
    fi

    # MAKEDEV for creating more device nodes at runtime
    if [ -d "$DESTDIR/dev" ]; then
        cp -a "$DESTDIR/dev/." "$STAGING/dev/"
    fi
else
    log "Full mode: copying entire DESTDIR"
    # Copy everything, preserving symlinks
    (cd "$DESTDIR" && tar cf - --exclude='./usr/include' --exclude='./METALOG' .) | \
        (cd "$STAGING" && tar xf -)

    # Ensure required directories exist
    mkdir -p "$STAGING"/{dev,tmp,var/run,var/tmp,mnt,root}

    # If /sbin/init doesn't exist (dynamically linked, won't work
    # without ld.elf_so), add a rescue fallback
    if [ ! -e "$STAGING/sbin/init" ] && [ -e "$STAGING/rescue/init" ]; then
        log "No /sbin/init found, symlinking to /rescue/init"
        ln -sf /rescue/init "$STAGING/sbin/init"
    fi
fi

# Create a minimal /etc/rc that just drops to a shell
if [ ! -e "$STAGING/etc/rc" ]; then
    cat > "$STAGING/etc/rc" <<'RCEOF'
#!/bin/sh
echo "Penumbra NetBSD — single-user"
exec /bin/sh
RCEOF
    chmod 755 "$STAGING/etc/rc"
fi

# Minimal /etc/fstab so `mount -u /` and friends can resolve the
# root device.  The kernel uses "root_device" as a placeholder in
# f_mntfromname until userland remounts via fstab.  Only create
# when DESTDIR didn't ship its own — a full distribution may
# have a pre-configured fstab we should respect.
CREATED_FSTAB=0
if [ ! -e "$STAGING/etc/fstab" ]; then
    cat > "$STAGING/etc/fstab" <<'FSTABEOF'
/dev/ld0f	/	ffs	rw	1 1
FSTABEOF
    chmod 644 "$STAGING/etc/fstab"
    CREATED_FSTAB=1
fi

# --- Build mtree spec for nbmakefs -----------------------------------------
# Combines METALOG (file permissions from build.sh) with device nodes
# generated by MAKEDEV -s.

SPECFILE="$TMPDIR/spec"
MAKEDEV_SCRIPT="$DESTDIR/dev/MAKEDEV"

# Fall back to build obj dir if DESTDIR copy isn't available
if [ ! -x "$MAKEDEV_SCRIPT" ]; then
    # Try the build obj directory
    SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
    dir="$SCRIPT_DIR"
    while [ "$dir" != "/" ]; do
        if [ -x "$dir/build/netbsd-obj/etc/MAKEDEV" ]; then
            MAKEDEV_SCRIPT="$dir/build/netbsd-obj/etc/MAKEDEV"
            break
        fi
        dir="$(dirname "$dir")"
    done
fi

# Build the spec file.  In full mode, use METALOG for correct permissions.
# In minimal mode, METALOG covers the full 350+ MB distribution which
# confuses nbmakefs sizing, so we build a minimal spec instead.
if [ "$MINIMAL" -eq 0 ] && [ -f "$DESTDIR/METALOG" ]; then
    log "Using METALOG for file permissions"
    # Add default uname/gname to symlinks that lack them (nbmakefs requires it)
    sed '/type=link/{ /uname=/!s/$/ uname=root gname=wheel/; }' \
        "$DESTDIR/METALOG" > "$SPECFILE"
else
    log "Building spec from staging directory"
    echo ". type=dir uname=root gname=wheel mode=0755" > "$SPECFILE"

    if [ "$MINIMAL" -eq 1 ] && [ -f "$DESTDIR/METALOG" ]; then
        # Extract only METALOG entries for paths that exist in staging.
        # Build a list of staged paths, then grep METALOG against it.
        (cd "$STAGING" && find . -print | sort) > "$TMPDIR/staged-paths"
        grep -v '^\./sbin/init \|^\./bin/sh ' "$DESTDIR/METALOG" | \
        sed '/type=link/{ /uname=/!s/$/ uname=root gname=wheel/; }' | \
        awk 'NR==FNR { paths[$0]=1; next } { p=$1; if (p in paths) print }' \
            "$TMPDIR/staged-paths" - >> "$SPECFILE"
    fi
fi

# Generate device nodes via MAKEDEV -s
# The 'std' target covers console/mem/random/etc; 'ld0' adds the
# logical-disk block/char nodes (ld0a..h + rld0a..h) that pmci
# exposes as the SD card.  Without ld0, /dev/ld0f doesn't exist
# and `mount -u /` cannot resolve the root device.
if [ -x "$MAKEDEV_SCRIPT" ]; then
    log "Generating device nodes via MAKEDEV -s std ld0"
    # MAKEDEV -s outputs mtree specs relative to /dev.
    # Prefix paths with ./dev/ and skip the "." root dir line.
    MACHINE=penumbra sh "$MAKEDEV_SCRIPT" -s std ld0 2>/dev/null | \
        grep -v '^[.] ' | sed 's,^\./,./dev/,' >> "$SPECFILE"
else
    log "Warning: MAKEDEV not found, no device nodes will be created"
fi

# Fixup entries we added (sbin/init symlink, etc/rc, directories)
cat >> "$SPECFILE" <<'EXTRAEOF'

# Directories that may not be in METALOG
./tmp type=dir uname=root gname=wheel mode=01777
./var type=dir uname=root gname=wheel mode=0755
./var/run type=dir uname=root gname=wheel mode=0755
./var/tmp type=dir uname=root gname=wheel mode=01777
./mnt type=dir uname=root gname=wheel mode=0755
./root type=dir uname=root gname=wheel mode=0755
EXTRAEOF

if [ "$MINIMAL" -eq 1 ]; then
    cat >> "$SPECFILE" <<'MINEOF'
./sbin/init type=link link=/rescue/init
./bin/sh type=link link=/rescue/sh
./etc/rc type=file uname=root gname=wheel mode=0755
MINEOF
fi

# Only add a spec entry for /etc/fstab if we created it ourselves;
# if DESTDIR shipped one, METALOG already covers it.
if [ "$CREATED_FSTAB" -eq 1 ]; then
    cat >> "$SPECFILE" <<'FSTABSPECEOF'
./etc/fstab type=file uname=root gname=wheel mode=0644
FSTABSPECEOF
fi

# --- Compute image size -----------------------------------------------------

STAGING_KB=$(du -sk "$STAGING" | cut -f1)
STAGING_MB=$(( (STAGING_KB + 1023) / 1024 ))

if [ -z "$SIZE_MB" ]; then
    # Auto-size: content + 20% slack + 2 MB for FFS metadata, minimum 32 MB
    SIZE_MB=$(( STAGING_MB + STAGING_MB / 5 + 2 ))
    if [ "$SIZE_MB" -lt 32 ]; then
        SIZE_MB=32
    fi
fi

if [ "$SIZE_MB" -lt "$STAGING_MB" ]; then
    echo "Error: image size ${SIZE_MB} MB too small for ${STAGING_MB} MB of content" >&2
    exit 1
fi

log "Staging: ${STAGING_MB} MB, image: ${SIZE_MB} MB"

# --- Create FFS image -------------------------------------------------------

IMAGE_BYTES=$(( SIZE_MB * 1024 * 1024 ))

log "Creating FFS image: $OUTPUT"
"$NBMAKEFS" -t ffs -s "$IMAGE_BYTES" -o v=1 \
    -F "$SPECFILE" -N "$DESTDIR/etc" \
    "$OUTPUT" "$STAGING"

echo "$OUTPUT: ${SIZE_MB} MB FFS rootfs (${STAGING_MB} MB content)"
