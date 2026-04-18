# Penumbra Development Guide

This document contains detailed instructions for building the Penumbra toolchain, running tests, and working with the NetBSD port.

## Prerequisites

- **Docker** (for Verilator simulation — no host install needed)
- **Python 3** (for assembler tools)
- **CMake, Ninja, ccache** (for building LLVM)
- **A C++ compiler** (g++ or clang++ for building LLVM itself)
- **zlib-dev** (for NetBSD host tools): `sudo apt install zlib1g-dev`

About 15 GB RAM recommended for LLVM debug builds (uses split DWARF to reduce memory pressure).

---

## 1. Toolchain Setup

### Building the LLVM toolchain

This builds a custom clang, lld, and llc with the Penumbra backend.

```sh
# One-time cmake configuration
cmake -G Ninja -S llvm/llvm -B build/llvm \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_INCLUDE_TESTS=ON \
  -DLLVM_BUILD_TESTS=ON \
  -DLLVM_PARALLEL_LINK_JOBS=2

# Build only the tools we need (much faster than a full build)
ninja -C build/llvm -j10 llc clang lld \
  llvm-mc llvm-ar llvm-nm llvm-objcopy llvm-objdump \
  llvm-readobj llvm-size llvm-strings
```

> **Low memory?** `-DLLVM_PARALLEL_LINK_JOBS=2` limits link parallelism.
> Debug builds with split DWARF need ~8 GB; without split DWARF, ~15 GB.
> Use `-j4` instead of `-j10` on machines with less RAM.

If you want the LLVM build tree elsewhere, pass `LLVM_PREFIX=/other/drive/penumbra-llvm` to make commands.

---

## 2. Hardware Simulation & Verification

### Running all hardware tests

Uses `pasm.py` assembler (no LLVM needed):

```sh
make test
```

### Module-specific simulation

```sh
make sim MOD=<name>      # Build & run testbench for a module
make wave MOD=<name>     # Open VCD waveform in GTKWave
```

### RTL vs ISS Simulation

- **ISS (Instruction Set Simulator):** Fast, instruction-level accuracy. Use for software development.
  ```sh
  make simulate
  ```
- **RTL (Verilator):** Slow, cycle-accurate. Use for hardware verification.
  ```sh
  make simulate-rtl
  ```

---

## 3. Hardware Development Workflow

When modifying the Penumbra hardware core, follow this general cycle:

### 1. Modify RTL or Microcode
- **RTL:** Edit modules in `hw/rtl/`. Follow the [Coding Standards](doc/internals/coding-standards.md).
- **Microcode:** Edit `hw/microcode/microcode.uasm`. See [Microcode Syntax](doc/internals/uasm-syntax.md).

### 2. Update Build Artifacts
If you modified microcode or the boot ROM, rebuild them:
```sh
make -C hw/rom
```
This updates `build/microcode.hex` and `build/program.hex`.

### 3. Verify via Simulation
- **Unit Test:** Run the testbench for the specific module you changed.
  ```sh
  make sim MOD=<module_name>
  ```
- **Regression:** Run the full hardware test suite to ensure no regressions.
  ```sh
  make test
  ```
- **Interactive:** Run the boot ROM on the cycle-accurate RTL model to verify system-level behavior.
  ```sh
  make simulate-rtl
  ```

### 4. Debugging
If a test fails, use the waveform viewer to inspect signals:
```sh
make wave MOD=<module_name>
```

---

## 4. LLVM Backend Testing

The Penumbra codegen backend uses LLVM's Lit framework with FileCheck. Tests live in `llvm/llvm/test/CodeGen/Penumbra/`.

```sh
# Run all backend tests
build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/

# Run a single test with verbose output
build/llvm/bin/llvm-lit -v llvm/llvm/test/CodeGen/Penumbra/alu.ll

# Regenerate CHECK lines after codegen changes
python3 llvm/llvm/utils/update_llc_test_checks.py \
  --llc-binary build/llvm/bin/llc \
  llvm/llvm/test/CodeGen/Penumbra/<test>.ll
```

---

## 5. Benchmarks

Benchmarks are PIE ELFs booted from the ROM.

```sh
# Quick run on ISS
make benchmark

# Cycle-accurate run on Verilator
make benchmark-rtl

# Override iteration count
make benchmark BENCH_ITERS=10000
```

---

## 6. NetBSD Porting

### Host Tools Setup (one-time)

```sh
# Create toolchain symlinks
sh netbsd/sys/arch/penumbra/toolchain-setup.sh

# Build NetBSD host tools
cd netbsd
./build.sh -U -j4 -m penumbra -a penumbra \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj \
  -T ../build/netbsd-tools \
  -D ../build/netbsd-dest \
  tools
cd ..
```

### Building the Kernel

Output goes to `build/netbsd-kernel/MINIMAL/`.

```sh
# 1. Generate kernel Makefile
build/netbsd-tools/bin/nbconfig \
  -b $PWD/build/netbsd-kernel/MINIMAL \
  -s $PWD/netbsd/sys \
  $PWD/netbsd/sys/arch/penumbra/conf/MINIMAL

# 2. Dependencies + build
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL depend
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL -j10
```

### Building the Bootloader

```sh
# Create objdir
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand obj

# Build (output: build/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF)
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand/boot
```
