#!/bin/sh
# Build one arm of a compiler A/B as a whole SD image: kernel, userland,
# every overlay and the FAT32 ELFs, with FLAG... added to every clang
# command.
#
# Usage: sw/tools/ab-image.sh [-k KERNCONF] [-f FLAVOR] [-j JOBS] NAME [FLAG...]
#
#   sw/tools/ab-image.sh base
#   sw/tools/ab-image.sh nosched -mllvm -enable-post-misched=false
#
# Build the baseline arm here too, with no flags.  Every arm is then a clean
# build by the same compiler, which an image from the regular tree does not
# promise: nothing there rebuilds when the compiler changes.
#
# Output: build/ab/NAME/, holding the image (penumbra[_FLAVOR].img), the
# NetBSD obj and dest dirs and the nbmake wrapper for them.  A second run
# with the same NAME replaces it.  KERNCONF defaults to GENERIC.
#
# The flags reach clang through CCC_OVERRIDE_OPTIONS, a change no make
# notices.  NetBSD builds into the arm's own dirs.  The overlays and the
# bare-metal programs build into the dirs a regular build uses, so the arm
# cleans them before and after; run no other build meanwhile.
set -eu
cd "$(dirname "$0")/../.."
R=$PWD

usage() {
	echo "usage: $0 [-k KERNCONF] [-f FLAVOR] [-j JOBS] NAME [FLAG...]" >&2
	exit 2
}

kernconf=GENERIC
flavor=
jobs=$(nproc)
while getopts k:f:j: opt; do
	case $opt in
	k) kernconf=$OPTARG ;;
	f) flavor=$OPTARG ;;
	j) jobs=$OPTARG ;;
	*) usage ;;
	esac
done
shift $((OPTIND - 1))
[ $# -ge 1 ] || usage
name=$1
shift
case $name in
.*|*[!A-Za-z0-9._-]*) echo "ab-image: bad arm name '$name'" >&2; exit 2 ;;
esac

# The variable's edits are separated by whitespace, so a flag cannot hold
# any.  -Wno-unused-command-line-argument keeps a link-only command under
# -Werror from failing on a compile flag.
unset CCC_OVERRIDE_OPTIONS
ccc=
if [ $# -gt 0 ]; then
	ccc="#"
	for f; do
		case $f in
		""|*[[:space:]]*)
			echo "ab-image: flag '$f' is empty or holds whitespace" >&2
			exit 2 ;;
		esac
		ccc="$ccc +$f"
	done
	export CCC_OVERRIDE_OPTIONS="$ccc +-Wno-unused-command-line-argument"
fi

A=$R/build/ab/$name
img=$A/penumbra${flavor:+_$flavor}.img
rm -rf "$A"
mkdir -p "$A"

cat > "$A/image.nfo" <<EOF
Kernel: $kernconf
Flavor: $flavor
Clang opts: $ccc
EOF

# Builds sharing their output dirs with the regular tree: NetBSD-hosted
# utilities have an `overlay' target, bare-metal productions for the FAT32
# partition a `stage' target, and benchmark/ holds the benchmark ELFs.
shared="benchmark $(git ls-files '*Makefile' |
    xargs grep -l -E '^(overlay|stage):' | xargs -n 1 dirname)"
clean_shared() {
	for d in $shared; do make -s -C "$d" clean; done
}
clean_shared
trap clean_shared EXIT
trap 'exit 130' INT TERM

nb() {
	(cd netbsd && MAKECONF=$R/minimal-mk.conf ./build.sh -j"$jobs" -U \
	    -m penumbra -O "$A/obj" -T "$R/build/netbsd-tools" -D "$A/dest" \
	    -w "$A/nbmake-penumbra" -V EXTERNAL_TOOLCHAIN="$R/build/llvm" "$@")
}
nb kernel="$kernconf"
nb distribution

make image FLAVOR="$flavor" \
    KERNEL="$A/obj/sys/arch/penumbra/compile/$kernconf/netbsd" \
    BOOT_ELF="$A/obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF" \
    DESTDIR="$A/dest" PENIMAGE="$img"

echo "ab-image: $img"
