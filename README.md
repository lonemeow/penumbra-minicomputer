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
- Boot ROM in C: autoconfig, SD card boot, interactive monitor
- Bus autoconfig, SPI controller, SD card read, MBR partition parsing
- 30+ hardware test programs passing
- NetBSD port started: machine headers + stage 1 bootloader skeleton

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

This builds a custom clang, lld, llvm-mc, and llvm-objcopy with the
Penumbra backend.  Takes 10-30 minutes depending on hardware.

```sh
mkdir -p build/llvm
cd build/llvm
cmake -G Ninja \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_INCLUDE_TESTS=ON \
  -DLLVM_BUILD_TESTS=ON \
  -DCMAKE_BUILD_TYPE=Debug \
  ../../llvm/llvm
ninja -j4 clang lld llvm-mc llvm-objcopy llc
cd ../..
```

> **Low memory?** Use `-j2` for the final link steps.  Debug builds with
> split DWARF need ~8 GB; without split DWARF, ~15 GB.

If you want the LLVM build tree elsewhere (e.g. a faster drive):

```sh
mkdir -p /other/drive/penumbra-llvm
cd /other/drive/penumbra-llvm
cmake -G Ninja \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  /path/to/penumbra-minicomputer/llvm/llvm
ninja -j4 clang lld llvm-mc llvm-objcopy
```

Then pass `LLVM_PREFIX=/other/drive/penumbra-llvm` to make commands.

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

**Current status:** Machine headers and stage 1 bootloader sources compile.
See `doc/netbsd/porting-status.md` for the full roadmap.

### Prerequisites

In addition to the LLVM toolchain above, you need:

- **zlib-dev** (for NetBSD host tools): `sudo apt install zlib1g-dev`

### Building (quick, standalone)

Compile the bootloader objects directly without the NetBSD build system:

```sh
make -C netbsd/sys/arch/penumbra/stand/boot -f Makefile.standalone
```

### Building (via build.sh)

For the full NetBSD build infrastructure (builds libsa, libkern, etc.):

```sh
# 1. Create toolchain symlinks (one-time setup)
sh netbsd/sys/arch/penumbra/toolchain-setup.sh

# 2. Build NetBSD host tools (use -u for incremental rebuilds after first time)
cd netbsd
./build.sh -U -j4 -m penumbra -a penumbra \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj \
  -T ../build/netbsd-tools \
  -D ../build/netbsd-dest \
  tools

# 3. Use nbmake-penumbra to build the bootloader
../build/netbsd-tools/bin/nbmake-penumbra -C sys/arch/penumbra/stand/boot
```

## License

This project is open source.  See [LICENSE](LICENSE) for details.
