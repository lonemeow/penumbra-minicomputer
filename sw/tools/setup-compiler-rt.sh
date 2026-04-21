#!/bin/sh
# Build compiler-rt builtins for Penumbra (one-time setup).
#
# Produces: build/compiler-rt-builtins/lib/linux/libclang_rt.builtins-penumbra.a
# Consumed by: `make test-compiler` (see CLAUDE.md "Compiler Correctness Tests")
#
# Requires the in-tree LLVM build at build/llvm/ (see llvm/llvm/lib/Target/Penumbra/CLAUDE.md).

set -eu

cd "$(dirname "$0")/../.."

if [ ! -x build/llvm/bin/clang ]; then
    echo "error: build/llvm/bin/clang not found — build LLVM first" >&2
    exit 1
fi

mkdir -p build/compiler-rt-builtins

cmake -G Ninja -S llvm/compiler-rt/lib/builtins -B build/compiler-rt-builtins \
  -DCMAKE_C_COMPILER="$PWD/build/llvm/bin/clang" \
  -DCMAKE_CXX_COMPILER="$PWD/build/llvm/bin/clang++" \
  -DCMAKE_AR="$PWD/build/llvm/bin/llvm-ar" \
  -DCMAKE_NM="$PWD/build/llvm/bin/llvm-nm" \
  -DCMAKE_RANLIB="$PWD/build/llvm/bin/llvm-ranlib" \
  -DCMAKE_C_COMPILER_TARGET=penumbra-unknown-none \
  -DCMAKE_CXX_COMPILER_TARGET=penumbra-unknown-none \
  -DCMAKE_ASM_COMPILER_TARGET=penumbra-unknown-none \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_C_FLAGS="-ffreestanding -nostdinc -isystem $PWD/build/llvm/lib/clang/22/include" \
  -DCMAKE_ASM_FLAGS="-ffreestanding -nostdinc -isystem $PWD/build/llvm/lib/clang/22/include" \
  -DCOMPILER_RT_BAREMETAL_BUILD=ON -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
  -DCOMPILER_RT_INCLUDE_TESTS=OFF -DCOMPILER_RT_USE_LIBCXX=OFF \
  -DCOMPILER_RT_BUILD_CRT=OFF -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
  -DCOMPILER_RT_BUILD_XRAY=OFF -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
  -DCOMPILER_RT_BUILD_PROFILE=OFF -DCOMPILER_RT_BUILD_MEMPROF=OFF \
  -DCOMPILER_RT_BUILD_ORC=OFF -DCOMPILER_RT_BUILD_GWP_ASAN=OFF \
  -DCOMPILER_RT_BUILD_CTX_PROFILE=OFF

ninja -C build/compiler-rt-builtins
