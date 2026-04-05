# Penumbra Minicomputer

A 32-bit RISC minicomputer designed from scratch and implemented on FPGA.
Inspired by classic machines like the Data General Eclipse and DEC VAX, but
with a clean load-store ISA.  The eventual goal is to run NetBSD and later
build the design from discrete 74xx logic chips.

## Status

The CPU is fully functional in simulation.  A C boot ROM compiled with
clang prints to UART and runs on the simulated CPU through the full
pipeline: CPU core, MMU (software-managed TLB), split I/D cache, and
memory-mapped I/O.

**What works:**
- All RTL modules implemented and tested (ALU, register file, sequencer,
  datapath, MMU, TLB, cache, UART, bus)
- 8 exception sources (IRQ, MMU faults, alignment, bus fault, BREAK,
  SYSCALL, privilege, illegal instruction)
- LLVM backend: clang compiles C, lld links, llvm-mc assembles
- Boot ROM in C: autoconfig, SD card boot, FAT32, interactive monitor
- Bus autoconfig, SPI controller, SD card read, MBR partition parsing
- 30+ hardware test programs passing, 26 LLVM codegen lit tests passing
- NetBSD kernel: all .o files compile at `-O0`, link stage reached

## Prerequisites

- **Docker** (for Verilator simulation — no host install needed)
- **Python 3** (for assembler tools)
- **CMake, Ninja, ccache** (for building LLVM)
- **A C++ compiler** (g++ or clang++ for building LLVM itself)

About 15 GB RAM recommended for LLVM debug builds (uses split DWARF to
reduce memory pressure).

## Building

### 1. Clone

```sh
git clone --recurse-submodules https://github.com/<user>/penumbra-minicomputer.git
cd penumbra-minicomputer
```

### 2. Build the LLVM toolchain

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
ninja -C build/llvm -j10 llc clang lld
```

> **Low memory?** `-DLLVM_PARALLEL_LINK_JOBS=2` limits link parallelism.
> Debug builds with split DWARF need ~8 GB; without split DWARF, ~15 GB.
> Use `-j4` instead of `-j10` on machines with less RAM.

If you want the LLVM build tree elsewhere, pass
`LLVM_PREFIX=/other/drive/penumbra-llvm` to make commands.

### 3. Verify the hardware (optional)

Run all hardware test programs (uses pasm.py assembler, no LLVM needed):

```sh
make test
```

### 4. Run the boot ROM

Build the C boot ROM with clang and launch the interactive simulator:

```sh
make simulate
```

This compiles `hw/rom/boot_rom.c` with crt0 startup, links it at
`0xFFFF_0000` (the reset vector), and boots the CPU in the Verilator
simulator with UART bridged to your terminal.

With a non-default LLVM location:

```sh
make simulate LLVM_PREFIX=/other/drive/penumbra-llvm
```

## Architecture

- **Word size:** 32-bit, little-endian
- **Registers:** 16 GPRs (R0=zero, R14=SP, R13=LR, R15=PC)
- **ISA:** 4 instruction formats (R/L/M/B), 2-operand destructive ALU,
  ARM-style NZCV condition flags
- **MMU:** Software-managed 64-entry 2-way set-associative TLB
- **Cache:** Split I/D, direct-mapped, write-through
- **Microcode:** 51-bit horizontal, 256-entry ROM

Full architecture docs in `doc/`.

## Repository Structure

```
hw/                Hardware design
  rtl/core/        CPU core (ALU, regfile, sequencer, datapath)
  rtl/mmu/         MMU and TLB
  rtl/soc/         SoC integration (boot ROM, UART, memory, cache)
  sim/             Testbenches and test programs
  microcode/       Microcode source
  rom/             Boot ROM (C source, crt0, linker script)
  tools/           Microcode assembler (uasm.py)
sw/tools/          ISA assembler (pasm.py), binary converter (bin2hex.py)
llvm/              LLVM backend (clang, lld, llvm-mc for Penumbra)
netbsd/            NetBSD 10.1 source tree (git subtree)
  sys/arch/penumbra/  Machine-dependent port (headers, bootloader)
doc/               Architecture specifications
```

## Make Targets

| Target | Description |
|--------|-------------|
| `make simulate` | Build C boot ROM and run interactive simulator |
| `make test` | Run all hardware test programs (pass/fail summary) |
| `make sim MOD=<name>` | Run a specific module's testbench |
| `make smoke` | Quick toolchain sanity check |
| `make wave MOD=<name>` | Open VCD waveform in GTKWave |
| `make clean` | Remove build artifacts |

## Testing

### Hardware tests

Run all CPU test programs (uses pasm.py, no LLVM needed):

```sh
make test
```

### LLVM backend tests

The Penumbra codegen backend has regression tests using LLVM's
[Lit](https://llvm.org/docs/CommandGuide/lit.html) framework with
[FileCheck](https://llvm.org/docs/CommandGuide/FileCheck.html) assertions.
Tests live in `llvm/llvm/test/CodeGen/Penumbra/`.

Run all backend tests:

```sh
build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/
```

Run a single test with verbose output:

```sh
build/llvm/bin/llvm-lit -v llvm/llvm/test/CodeGen/Penumbra/alu.ll
```

After changing codegen, regenerate the expected CHECK lines:

```sh
python3 llvm/llvm/utils/update_llc_test_checks.py \
  --llc-binary build/llvm/bin/llc \
  llvm/llvm/test/CodeGen/Penumbra/<test>.ll
```

Review the diff to make sure the output changes are intentional.

## NetBSD Port

The goal is to run NetBSD on Penumbra.  The NetBSD 10.1 source tree is
included as a git subtree under `netbsd/`.  Machine-dependent port files
live in `netbsd/sys/arch/penumbra/`.

**Current status:** All kernel .o files compile at `-O0`.  Link stage
reached (fails with expected undefined symbols from stubs).
See `netbsd/sys/arch/penumbra/CLAUDE.md` for detailed port context.

### Prerequisites

In addition to the LLVM toolchain above, you need:

- **zlib-dev** (for NetBSD host tools): `sudo apt install zlib1g-dev`

### Building NetBSD host tools (one-time)

```sh
# Create toolchain symlinks
sh netbsd/sys/arch/penumbra/toolchain-setup.sh

# Build NetBSD host tools (use -u for incremental rebuilds after first time)
cd netbsd
./build.sh -U -j4 -m penumbra -a penumbra \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj \
  -T ../build/netbsd-tools \
  -D ../build/netbsd-dest \
  tools
cd ..
```

### Building the kernel

All commands from the project root.  Build output goes to
`build/netbsd-kernel/MINIMAL/` (out of source tree).

```sh
# 1. Generate kernel Makefile (re-run after changing conf/ files)
build/netbsd-tools/bin/nbconfig \
  -b $PWD/build/netbsd-kernel/MINIMAL \
  -s $PWD/netbsd/sys \
  $PWD/netbsd/sys/arch/penumbra/conf/MINIMAL

# 2. Dependencies + build
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL depend
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL -j10
```

### Building the bootloader (standalone)

```sh
make -C netbsd/sys/arch/penumbra/stand/boot -f Makefile.standalone
```

## License

This project is open source.  See [LICENSE](LICENSE) for details.
