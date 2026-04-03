#!/bin/sh
#
# toolchain-setup.sh — Create prefixed symlinks for NetBSD build.sh
#
# NetBSD's EXTERNAL_TOOLCHAIN mechanism expects tools named
# penumbra-unknown-none-{clang,ar,ld,...} in $EXTERNAL_TOOLCHAIN/bin/.
# This script creates those symlinks pointing to our LLVM tools.
#
# Usage:
#   sh toolchain-setup.sh [/path/to/llvm/build]
#
# Default LLVM path: ../../build/llvm (relative to this script)

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/../../../.." && pwd)
LLVM_PREFIX="${1:-$ROOT/build/llvm}"
BIN="$LLVM_PREFIX/bin"
PREFIX="penumbra-unknown-none"

if [ ! -x "$BIN/clang" ]; then
    echo "error: $BIN/clang not found" >&2
    exit 1
fi

# Map GNU-style tool names to LLVM tool names
create_link() {
    local gnu_name="$1"
    local llvm_name="$2"
    local target="$BIN/$PREFIX-$gnu_name"

    if [ -e "$target" ] && [ ! -L "$target" ]; then
        echo "skip: $target exists and is not a symlink"
        return
    fi
    ln -sf "$llvm_name" "$target"
    echo "  $PREFIX-$gnu_name -> $llvm_name"
}

echo "Creating toolchain symlinks in $BIN/"

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

echo "Done. Use with:"
echo "  cd netbsd && ./build.sh -m penumbra -a penumbra \\"
echo "    -V EXTERNAL_TOOLCHAIN=$LLVM_PREFIX tools"
