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

Penumbra has two simulators: a fast native C++ **ISS** (`sw/sim/penumbra_iss.cpp`, no Docker, no LLVM) and a cycle-accurate **Verilator RTL** simulation (Docker-based). Most workflows use the ISS; reach for RTL when you care about cycle counts, bus timing, or are debugging actual hardware behavior.

### Running tests

```sh
make smoke           # Toolchain smoke test (trivial adder)
make test-iss        # Run all hw/sim/programs/test_*.s on ISS — fast, no Docker
make test            # Same suite on RTL via Docker — slow but cycle-accurate
make test-modules    # Run all module-level Verilator testbenches (alu, regfile, …)
make test-all        # test + test-modules
```

`make test-iss` is the right default during development. Run `make test` (or `make test-all`) before committing RTL changes.

### Interactive simulation

```sh
make simulate                                # ISS (fast)
make simulate-rtl                            # Verilator RTL (cycle-accurate)
```

Both targets accept the same runtime knobs:

| Variable          | Effect                                                                          |
|-------------------|---------------------------------------------------------------------------------|
| `SDCARD=img`      | Attach an SD-card image (see § 7).                                              |
| `TRACE=log`       | Dump per-instruction PC/SR/R1–R14 trace.                                        |
| `LLVM_PREFIX=…`   | Use a non-default LLVM build tree.                                              |

ISS-only:

| Variable          | Effect                                                                          |
|-------------------|---------------------------------------------------------------------------------|
| `RAW=1`           | Raw-TTY mode (job control passes through; Ctrl-A is the escape — Ctrl-A H for help). |

RTL-only:

| Variable          | Effect                                                                          |
|-------------------|---------------------------------------------------------------------------------|
| `INTERACTIVE=0`   | Non-interactive mode for piped input (e.g. `echo break \| make simulate-rtl INTERACTIVE=0`). |
| `TRACE_WINDOW=N`  | Rolling last-N-instruction trace (caps disk use on long boots).                 |
| `HALT_ON='pat'`   | Auto-exit when UART output matches the pattern.                                 |
| `STDIN_FILE=f`    | Replay keystrokes from a file at fixed cycle cadence, then fall back to live stdin. |

Type `break` (or `b`) at the ROM monitor to halt the simulator cleanly.

### Module-specific simulation

```sh
make sim MOD=<name>      # Build & run testbench for one module (expects hw/sim/tb_<name>.cpp)
make wave MOD=<name>     # Open VCD waveform in GTKWave
```

If you suspect stale Verilator output (WSL2 mtimes), `rm -rf build/<mod>.verilator build/V<mod>` and rebuild.

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

## 5. Compiler Correctness Testing

Penumbra uses a subset of the LLVM `SingleSource` test suite (including GCC C-Torture) to verify code generation. These tests run on the ISS in a special `+hosted` mode with a minimal `libc` harness.

### Building compiler-rt builtins

The tests require `compiler-rt` to provide soft-float and 64-bit integer operations. A helper script handles the cmake invocation:

```sh
sw/tools/setup-compiler-rt.sh
```

Output: `build/compiler-rt-builtins/lib/linux/libclang_rt.builtins-penumbra.a`. Re-run after upgrading clang.

### Running compiler tests

Tests are executed in parallel and verified against `.reference_output` files where available.

```sh
# Run the full suite at -O2 (default)
make test-compiler

# Run at a different optimization level
make test-compiler OPT="-Os"

# Run a specific subset or single test
make test-compiler COMPILER_TESTS="test/compiler/llvm-test-suite/UnitTests/2002-05-02-ArgumentTest.c"
```

A full report is written to `build/test-compiler-report.txt`.

---

## 6. Benchmarks

Two benchmark families live under `benchmark/`:

**Bare-metal** (PIE ELFs booted from ROM via `boot sd:0,0/<NAME>.ELF`):
- `DHRYSTON.ELF` — Dhrystone 2.1 (DMIPS / CPI).
- `MEMBENCH.ELF` — cached + uncached memory throughput / latency at W/H/B sizes.
- `MEMTEST.ELF` — SDRAM correctness walk (six patterns; round-trip / walking-1 / etc.).

```sh
make benchmark                 # ISS (fast, all three benches)
make benchmark-rtl             # Verilator (cycle-accurate, slower; memtest dominates)
make benchmark BENCH_ITERS=100 # Override Dhrystone iteration count
make benchmark COPT="-Os"      # Override benchmark optimization level
```

> **Real numbers come from the FPGA**, not the ISS or Verilator. Use the sim runs to validate that benchmarks build and execute correctly; flash to the ULX3S (§ 9) and re-run for the numbers that go in `benchmark/.../BASELINE.md`.

**NetBSD-hosted** (`pbench` — runs under the real userland against libc, dynamic + static):

```sh
make benchmark-netbsd                              # Build pbench{,-static} for NetBSD
make sdimage-rootfs ROOTFS_FULL=1                  # Bake into /usr/local/bin/ on rootfs
make simulate SDCARD=build/boot.img                # Boot, log in, run pbench
```

Inside the running NetBSD: `pbench list`, `pbench libc memcpy`, etc. Use `pbench -o FILE` to dump machine-readable `RESULT key=value` lines. Baseline numbers in `benchmark/netbsd-bench/BASELINE.md`.

---

## 7. NetBSD Porting

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

```sh
cd netbsd
MAKECONF=${PWD}/../minimal-mk.conf ./build.sh -j10 -U -m penumbra \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  kernel=MINIMAL
cd ..
```

Output: `build/netbsd-obj/sys/arch/penumbra/compile/MINIMAL/netbsd`. `build.sh kernel=...` runs `nbconfig` + `make depend` + `make` itself; `-U` (MKUPDATE) keeps subsequent runs incremental.

### Building the Bootloader

```sh
# Create objdir
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand obj

# Build (output: build/netbsd-obj/sys/arch/penumbra/stand/boot/PENBOOT.ELF)
build/netbsd-tools/bin/nbmake-penumbra -C netbsd/sys/arch/penumbra/stand/boot
```

### Building the Userland

The full distribution (libraries + programs + `etc.penumbra`) builds with stock `build.sh`:

```sh
cd netbsd
./build.sh -u -j4 -m penumbra -a penumbra \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest \
  distribution
cd ..
```

`-u` makes the build incremental — only the first run is slow.

---

## 8. SD Card Images and Booting NetBSD

The Penumbra ROM boots from FAT32 on an SD card; the NetBSD kernel mounts an FFS root from a second partition. Image creation is wrapped:

```sh
make sdimage                          # Boot partition only (FAT32: bootloader + kernel)
make sdimage-rootfs                   # Boot + minimal FFS root (rescue + lib + etc, ~86 MB)
make sdimage-rootfs ROOTFS_FULL=1     # Boot + full FFS root from build/netbsd-dest/
```

Output: `build/boot.img` (two MBR partitions). The rootfs variants pre-write a `boot.cfg` that selects `root=ld0f`, so `boot sd:0,0` reaches single-user shell with no further interaction.

End-to-end recipe (assumes kernel + bootloader + userland already built):

```sh
make sdimage-rootfs ROOTFS_FULL=1
make simulate SDCARD=build/boot.img
```

Image creation requires the NetBSD cross-tools (`nbfdisk`, `nbmakefs`) — i.e. the host-tools step from § 7 must have run.

---

## 9. FPGA Synthesis (ULX3S)

FPGA synthesis uses the OSS CAD Suite (Yosys + nextpnr-ecp5 + ecppack + fujprog) via Docker. The wrappers in `hw/tools/oss-cad-suite/bin/` make them usable as normal commands:

```sh
export PATH="$PWD/hw/tools/oss-cad-suite/bin:$PATH"
```

### Build & flash

```sh
make fpga TOP=ulx3s_top              # Full flow: sv2v → yosys → nextpnr → ecppack
make flash TOP=ulx3s_top             # Build + flash to ULX3S over USB (fujprog)
make fpga-lint TOP=ulx3s_top         # Verilator lint check on FPGA sources
```

`TOP` defaults to `ulx3s_hello` (a minimal smoke top). For the full system, use `TOP=ulx3s_top`. Other tops in `hw/rtl/fpga/`: `ulx3s_regtest`, `ulx3s_utest`.

### Reading the build report

```sh
make timing TOP=ulx3s_top                  # Pretty-print fmax + top critical paths
make timing TOP=ulx3s_top TOP_N=10         # ...top 10 instead of 5
make fanout TOP=ulx3s_top                  # High-fanout nets (router-congestion diagnosis)
make fanout TOP=ulx3s_top FANOUT_N=30 FANOUT_MIN=20
```

Both targets read whatever the last `make fpga` left in `build/`; they don't trigger a rebuild.

### SDRAM phase tuning

The SDRAM clock-pin phase is built into the bitstream filename via `PHASE_DEG`:

```sh
make fpga TOP=ulx3s_top PHASE_DEG=270      # Default — step-4 baseline
```

Valid values: `0, 45, 90, 135, 180, 225, 270, 315`. See `doc/internals/sdram-controller.md` for the bring-up sweep procedure.

### Serial console

After flashing: `/dev/ttyUSB0` at **115200 8N1**. The boot ROM accepts `boot sd:0,0` to load `PENBOOT.ELF` from FAT32.
