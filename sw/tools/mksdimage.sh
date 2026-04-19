#!/bin/bash
# mksdimage.sh — Create a test SD card image for the Penumbra boot chain.
#
# Uses NetBSD cross-tools (nbfdisk, nbmakefs) so no host packages needed.
#
# Disk layout (from doc/system/boot-protocol.md):
#   Sector 0:           MBR partition table
#   Sectors 1–2047:     Partition gap (unused)
#   Sectors 2048+:      Partition 1: FAT32 (boot loader + kernel)
#
# The ROM's "boot" command mounts the FAT32 partition and loads
# the file named LOADER from the root directory.
#
# Usage:
#   mksdimage.sh -o disk.img [options]
#
# Options:
#   -o FILE         Output image path (required)
#   -s SIZE_MB      Total image size in MB (default: 64, minimum ~34 for FAT32)
#   -b SIZE_MB      Boot partition size in MB (default: all remaining space,
#                   or 16 MB when -r is also given)
#   -1 FILE         Binary to write into partition gap (rarely needed)
#   -2 FILE         Boot loader, placed as PENBOOT.ELF on FAT32
#   -k FILE         Kernel image, placed as PENUMBRA on FAT32
#   -r FILE         Root filesystem image (FFS), placed as partition 2
#   -e DIR          Extra directory: copy all contents onto FAT32 root
#   -T TOOLDIR      NetBSD tools directory (default: auto-detect)
#   -v              Verbose output
#
# Examples:
#   # Image with boot loader for ROM "boot sd:0,0" command:
#   mksdimage.sh -o disk.img -2 loader.bin
#
#   # Full image with loader and kernel:
#   mksdimage.sh -o disk.img -2 loader.bin -k netbsd
#
#   # Boot + FFS root filesystem:
#   mksdimage.sh -o disk.img -2 loader.bin -k netbsd -r rootfs.img

set -euo pipefail

# --- Defaults ---------------------------------------------------------------

OUTPUT=""
SIZE_MB=""
BOOT_MB=""
STAGE1=""
STAGE2=""
KERNEL=""
ROOTFS=""
EXTRA_DIR=""
TOOLDIR=""
VERBOSE=0

# --- Parse arguments --------------------------------------------------------

usage() {
    sed -n '2,/^$/{ s/^# \?//; p; }' "$0"
    exit 1
}

while getopts "o:s:b:1:2:k:r:e:T:vh" opt; do
    case $opt in
        o) OUTPUT="$OPTARG" ;;
        s) SIZE_MB="$OPTARG" ;;
        b) BOOT_MB="$OPTARG" ;;
        1) STAGE1="$OPTARG" ;;
        2) STAGE2="$OPTARG" ;;
        k) KERNEL="$OPTARG" ;;
        r) ROOTFS="$OPTARG" ;;
        e) EXTRA_DIR="$OPTARG" ;;
        T) TOOLDIR="$OPTARG" ;;
        v) VERBOSE=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

if [ -z "$OUTPUT" ]; then
    echo "Error: -o FILE is required" >&2
    usage
fi

# --- Locate NetBSD tools ----------------------------------------------------

find_tooldir() {
    local script_dir
    script_dir="$(cd "$(dirname "$0")" && pwd)"

    # Walk up to find the project root (contains build/)
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

# Find fdisk — it may be prefixed with the target triple
NBFDISK=""
for name in nbfdisk penumbra-unknown-netbsd-fdisk; do
    if [ -x "$TOOLDIR/$name" ]; then
        NBFDISK="$TOOLDIR/$name"
        break
    fi
done
if [ -z "$NBFDISK" ]; then
    echo "Error: fdisk not found in $TOOLDIR" >&2
    echo "Build it:  nbmake-penumbra -C netbsd/tools/fdisk" >&2
    exit 1
fi

NBMAKEFS="$TOOLDIR/nbmakefs"
if [ ! -x "$NBMAKEFS" ]; then
    echo "Error: nbmakefs not found in $TOOLDIR" >&2
    exit 1
fi

log() { [ "$VERBOSE" -eq 1 ] && echo "  $*" >&2 || true; }

# --- Compute geometry -------------------------------------------------------

SECTOR=512
GAP_START=1          # first sector after MBR
GAP_END=2047         # last sector of gap (inclusive)
PART1_START=2048     # 1 MB aligned

# If root filesystem given, auto-size the total image if not specified
if [ -n "$ROOTFS" ]; then
    ROOTFS_BYTES=$(stat -c%s "$ROOTFS" 2>/dev/null || stat -f%z "$ROOTFS")
    ROOTFS_MB=$(( (ROOTFS_BYTES + 1048575) / 1048576 ))
    : "${BOOT_MB:=34}"  # FAT32 minimum ~34 MB (65525 clusters)
    : "${SIZE_MB:=$(( BOOT_MB + ROOTFS_MB + 2 ))}"  # +2 for MBR gap + alignment
else
    : "${BOOT_MB:=}"
    : "${SIZE_MB:=64}"
fi

TOTAL_SECTORS=$(( SIZE_MB * 1024 * 1024 / SECTOR ))

if [ -n "$ROOTFS" ]; then
    # Two partitions: FAT32 boot + FFS root
    PART1_SECTORS=$(( BOOT_MB * 1024 * 1024 / SECTOR ))
    PART2_START=$(( PART1_START + PART1_SECTORS ))
    PART2_SECTORS=$(( TOTAL_SECTORS - PART2_START ))

    log "Image: ${SIZE_MB} MB, ${TOTAL_SECTORS} sectors"
    log "FAT32 boot:  start=${PART1_START}, size=${PART1_SECTORS} (${BOOT_MB} MB)"
    log "FFS root:    start=${PART2_START}, size=${PART2_SECTORS} (${ROOTFS_MB} MB)"
else
    # Single partition: FAT32 fills remainder
    PART1_SECTORS=$(( TOTAL_SECTORS - PART1_START ))

    log "Image: ${SIZE_MB} MB, ${TOTAL_SECTORS} sectors"
    log "FAT32 partition: start=${PART1_START}, size=${PART1_SECTORS}"
fi

# --- Create empty image -----------------------------------------------------

dd if=/dev/zero of="$OUTPUT" bs=$SECTOR count="$TOTAL_SECTORS" status=none
log "Created blank image: $OUTPUT"

# --- Write MBR via NetBSD fdisk ---------------------------------------------

# Initialize MBR, then add partition 0 as FAT32-LBA (sysid 11)
"$NBFDISK" -Ffi "$OUTPUT" >/dev/null 2>&1
"$NBFDISK" -Ffu -0 -s "11/${PART1_START}/${PART1_SECTORS}" "$OUTPUT" >/dev/null 2>&1
log "MBR partition 0: FAT32-LBA"

# Add partition 1 as NetBSD/FFS (sysid 169) if root filesystem given
if [ -n "$ROOTFS" ]; then
    "$NBFDISK" -Ffu -1 -s "169/${PART2_START}/${PART2_SECTORS}" "$OUTPUT" >/dev/null 2>&1
    log "MBR partition 1: NetBSD (FFS)"
fi

# --- Write stage 1 into partition gap ---------------------------------------

if [ -n "$STAGE1" ]; then
    local_size=$(stat -c%s "$STAGE1" 2>/dev/null || stat -f%z "$STAGE1")
    gap_bytes=$(( (GAP_END - GAP_START + 1) * SECTOR ))
    if [ "$local_size" -gt "$gap_bytes" ]; then
        echo "Error: stage 1 ($local_size bytes) exceeds gap ($gap_bytes bytes)" >&2
        exit 1
    fi
    dd if="$STAGE1" of="$OUTPUT" bs=$SECTOR seek=$GAP_START conv=notrunc status=none
    log "Stage 1 written to sectors ${GAP_START}–${GAP_END} ($local_size bytes)"
fi

# --- Build FAT32 filesystem image ------------------------------------------

BOOTIMGTMP=`mktemp -d`

cleanup_temp() {
    rm -r "$BOOTIMGTMP"
}

trap cleanup_temp EXIT

BOOTFSROOT="$BOOTIMGTMP/fsroot"
mkdir $BOOTFSROOT

if [ -n "$STAGE2" ]; then
    log "Copying $STAGE2 -> PENBOOT.ELF"
    cp "$STAGE2" "$BOOTFSROOT/PENBOOT.ELF"
fi

if [ -n "$KERNEL" ]; then
    log "Copying $KERNEL -> netbsd"
    cp "$KERNEL" "$BOOTFSROOT/netbsd"
fi

if [ -n "$ROOTFS" ]; then
    log "Writing boot.cfg (root=ld0f)"
    printf 'root=ld0f\n' > "$BOOTFSROOT/boot.cfg"
fi

if [ -n "$EXTRA_DIR" ]; then
    log "Copying $EXTRA_DIR"
    cp -r "$EXTRA_DIR/." "$BOOTFSROOT/"
fi

log "Creating boot filesystem (FAT32)"
boot_image_size=$(( PART1_SECTORS * 512 ))
"$NBMAKEFS" -t msdos -o F=32 -o c=1 -s $boot_image_size "$BOOTIMGTMP/boot.img" "$BOOTFSROOT"

log "Inserting boot filesystem to SD card image"
dd if="$BOOTIMGTMP/boot.img" of="$OUTPUT" bs=$SECTOR seek=$PART1_START conv=notrunc status=none

# --- Write FFS root filesystem into partition 2 ----------------------------

if [ -n "$ROOTFS" ]; then
    log "Inserting FFS root filesystem to partition 2"
    dd if="$ROOTFS" of="$OUTPUT" bs=$SECTOR seek=$PART2_START conv=notrunc status=none
fi

# --- Done -------------------------------------------------------------------

echo "$OUTPUT: ${SIZE_MB} MB SD image ready"
if [ "$VERBOSE" -eq 1 ]; then
    "$NBFDISK" -Fv "$OUTPUT" 2>&1
fi
