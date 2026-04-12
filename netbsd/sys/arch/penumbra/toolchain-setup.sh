#!/bin/sh
#
# toolchain-setup.sh — Create prefixed symlinks for NetBSD build system
#
# Creates penumbra-unknown-netbsd-{clang,ar,ld,...} symlinks in the LLVM
# build's bin/ directory, so EXTERNAL_TOOLCHAIN can find them.
#
# Usage:
#   sh toolchain-setup.sh [/path/to/llvm/build]
#
# Default LLVM path: ../../build/llvm (relative to project root)

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../../../.." && pwd)
LLVM_PREFIX="${1:-$ROOT/build/llvm}"
BIN="$LLVM_PREFIX/bin"
PREFIX="penumbra-unknown-netbsd"

if [ ! -x "$BIN/clang" ]; then
    echo "error: $BIN/clang not found" >&2
    exit 1
fi

MISSING=0

# Map GNU-style tool names to LLVM tool names
create_link() {
    local gnu_name="$1"
    local llvm_name="$2"
    local target="$BIN/$PREFIX-$gnu_name"

    if [ ! -e "$BIN/$llvm_name" ]; then
        echo "error: $BIN/$llvm_name not found — did you build it?" >&2
        echo "  hint: ninja -C $(dirname "$BIN") llvm-mc llvm-ar llvm-nm llvm-objcopy llvm-objdump llvm-readobj llvm-size llvm-strings" >&2
        MISSING=1
        return
    fi
    if [ -e "$target" ] && [ ! -L "$target" ]; then
        echo "skip: $target exists and is not a symlink"
        return
    fi
    ln -sf "$llvm_name" "$target"
    echo "  $PREFIX-$gnu_name -> $llvm_name"
}

echo "Creating toolchain symlinks in $TARGET_DIR/"

# Compiler
create_link clang      clang
create_link clang++    clang++
create_link clang-cpp  clang-cpp

# Binutils (LLVM equivalents)
create_link ar         llvm-ar
create_link as         clang          # use clang as assembler
create_link ld         ld.lld
create_link nm         llvm-nm
create_link objcopy    llvm-objcopy
create_link objdump    llvm-objdump
create_link ranlib     llvm-ranlib
create_link readelf    llvm-readelf
create_link size       llvm-size
create_link strings    llvm-strings
create_link strip      llvm-strip

if [ "$MISSING" -ne 0 ]; then
    echo "" >&2
    echo "error: some LLVM tools are missing — aborting without creating broken symlinks" >&2
    exit 1
fi

echo ""
echo "Done. Use with:"
echo "  cd netbsd && ./build.sh -U -j4 -m penumbra tools \\"
echo "    -V EXTERNAL_TOOLCHAIN=$LLVM_PREFIX \\"
echo "    -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest"
