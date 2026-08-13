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
make test-iss        # hw/sim/programs/isa/ conformance suite on ISS — fast, no Docker
make test            # isa/ + per-core suite on RTL via Docker (CORE=penumbra2 default)
make test CORE=penumbra1          # same, against the gen1 microcoded core
make test-prog CORE=penumbra2 PROG=test_smoke   # one program
make test-modules    # Run all module-level Verilator testbenches (alu, regfile, …)
make test-all        # test + test-modules
```

Suite layout and the `; RUNNER:` / `; REQUIRES:` program header tags
are specified in `doc/internals/build-system.md`.

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
| `USBDEV=type:cfg` | Attach a simulated USB device to the USBHC port, e.g. `USBDEV=disk:disk.img` (see § 8). |

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
make image                                         # Stage all custom utilities into the rootfs
make simulate SDCARD=build/boot.img                # Boot, log in, run pbench
```

`make image` automatically builds and overlays the whole benchmark
suite into `/usr/local/bin/` (see § 8). Inside the running NetBSD: `pbench list`, `pbench libc memcpy`, etc. Use `pbench -o FILE` to dump machine-readable `RESULT key=value` lines. Baseline numbers in `benchmark/netbsd-bench/BASELINE.md`.

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
  kernel=GENERIC.DEBUG
cd ..
```

Output: `build/netbsd-obj/sys/arch/penumbra/compile/GENERIC.DEBUG/netbsd`. `build.sh kernel=...` runs `nbconfig` + `make depend` + `make` itself.

`-U` sets `MKUNPRIVED`, which `build.sh` requires of any build not run as
root; it is unrelated to incrementality. The flag that makes a run
incremental is `-u` (`MKUPDATE`), which skips the initial `make cleandir`.
Leaving `-u` off gives the clean rebuild these commands intend.

Three configs exist, per NetBSD convention: **`GENERIC`** (full device
set, consistency checks off — demo and performance images),
**`GENERIC.DEBUG`** (GENERIC plus `DIAGNOSTIC` — the development
default; asserts have caught enough to be worth their cost), and
**`MINIMAL`** (the smallest bootable kernel — fast smoke builds for MD
changes, and documentation of the port's true floor). The SD-image
targets take `KERNCONF=<config>` (default `GENERIC.DEBUG`) to select
which kernel they carry.

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
./build.sh -U -j4 -m penumbra -a penumbra \
  -V EXTERNAL_TOOLCHAIN=$PWD/../build/llvm \
  -O ../build/netbsd-obj -T ../build/netbsd-tools -D ../build/netbsd-dest \
  distribution
cd ..
```

Add `-u` if you want an incremental run; without it the build starts from
`make cleandir`, which is what you want when the toolchain changed.

#### Clean rebuilds

`-u` controls object directories; it has no bearing on DESTDIR. Leaving
it off rebuilds all the code but leaves DESTDIR exactly as the previous
build left it, `METALOG` included. Pick the depth you actually want:

| Goal | Before `distribution` |
|------|------------------------|
| Rebuild the code, keep DESTDIR, honest `METALOG` | `rm -f build/netbsd-dest/METALOG*` |
| Pristine DESTDIR, keep the host tools | `rm -rf build/netbsd-dest` |
| Rebuild the host tools too | use `build.sh -r` instead |

The middle row is the usual one: it drops files whose sources no longer
exist, which the first row leaves orphaned, while keeping the expensive
`build/netbsd-tools`. `distribution` repopulates an empty DESTDIR on its
own. `-r` removes TOOLDIR *and* DESTDIR and rebuilds `nbmake` first, so
it still needs no separate `tools` run — it is just far slower.

The last thing `distribution` does is regenerate the set lists under
`build/netbsd-dest/etc/mtree/`. Those lists are the file inventory every
image build reads, they are derived from `DESTDIR/METALOG`, and `makefs`
refuses any file whose size disagrees with its entry.

`METALOG` is append-only: a reinstalled binary is recorded as an
*additional* entry beside the original, so it accumulates across builds.
Nothing in a clean build removes it. `cleandir` only wipes object
directories, and the tree's own `clean_METALOG` target delegates to the
step that would delete it only `.if ${MKUPDATE} != "no"` — that is, only
under `-u`. A clean build therefore inherits every previous build's
entries, and `MKUPDATE=no` also disables the pass that keeps the newest
entry per path. Which duplicate survives is then decided by the `sort`
that feeds `mtree -M`, and `mtree` keeps the last line it reads: for
entries differing only in `size=` and `sha256=`, that is whichever string
compares greatest. The winner bears no relation to which build installed
the file.

The same thing happens, with or without `-u`, when something installs
into DESTDIR outside a full build — a targeted
`nbmake-penumbra -C some/dir install`, or an interrupted run.

To repair a DESTDIR already in that state without rebuilding it,
regenerate just the lists:

```sh
rm -f build/netbsd-dest/METALOG.sanitised
MAKECONF=$PWD/minimal-mk.conf build/netbsd-tools/bin/nbmake-penumbra \
  -C netbsd/distrib/sets makesetfiles \
  DESTDIR=$PWD/build/netbsd-dest \
  RELEASEDIR=$PWD/build/netbsd-release \
  MKUNPRIVED=yes MKUPDATE=yes
```

`MKUPDATE=yes` there selects the newest entry per path rather than the
oldest; it is a property of that one command, not a request for an
incremental build. Deleting `METALOG.sanitised` first is required because
it is rebuilt from a plain mtime dependency on `METALOG`, and a build
routinely writes both within the same second.

---

## 8. SD Card Images and Booting NetBSD

The Penumbra ROM boots from FAT32 on an SD card; the NetBSD kernel mounts an FFS root from a second partition. Images are built by NetBSD's own `distrib/utils/embedded/mkimage`, driven by a board conf per flavor:

```sh
make image                            # single-user shell
make image FLAVOR=multiuser           # /etc/rc, getty on every tty marked on
make image FLAVOR=kiosk               # the exhibit launcher on the display
```

`FLAVOR` names a conf under `netbsd/distrib/utils/embedded/conf/`, which all carry a `penumbra_` prefix the flavor name leaves off; adding a flavor means adding a conf and nothing else. Output is `build/penumbra[_FLAVOR].img`.

Each image carries a `boot.cfg` selecting `root=ld0f`, so `boot sd:0,0` needs no further interaction, plus the bare-metal benchmark ELFs (`DHRYSTON.ELF`, `MEMTEST.ELF`, `MEMBENCH.ELF`) on the FAT32 partition — `boot sd:0,0/DHRYSTON.ELF` runs one straight from ROM on the same card.

End-to-end recipe (assumes kernel + bootloader + userland already built):

```sh
make image
make simulate SDCARD=build/penumbra.img
```

Image creation requires the NetBSD cross-tools (`nbfdisk`, `nbmakefs`) — i.e. the host-tools step from § 7 must have run.

### Simulated USB devices

The ISS models the `CLASS_USBHC` port with a pluggable simulated device
behind it. `USBDEV=<type>:<config>` selects the device's personality;
`disk:<image>` is a USB mass-storage disk backed by a raw image file:

```sh
cp build/boot.img build/usbdisk.img     # any raw image works
make simulate SDCARD=build/boot.img USBDEV=disk:build/usbdisk.img RAW=1
```

NetBSD enumerates it through the full stack — `umass0` → `scsibus0` →
`sd0` — and the disk behaves like any other: `usbdevs -v` shows the
bus, `disklabel sd0` reads the label, and partitions mount normally
(`mount /dev/sd0f /mnt` for an image with an FFS partition).

**Hot-plugging:** under `RAW=1`, `Ctrl-A U` toggles the device's plug
state at runtime. Unplugging tears the device tree down (`sd0` …
`umass0` detach, in-flight transfers abort); replugging re-enumerates
the device from scratch. This is the way to exercise the host stack's
disconnect and recovery paths.

Without `USBDEV=` the port carries a bare enumerable vendor-class
device (what the enumeration test drives). The ISS rejects unknown
device types and lists the available ones. New personalities plug into
the device-function seam in `hw/sim/usb_device_sim.h` — the wire layer
stays shared; a personality supplies descriptors, control requests,
and bulk-transfer behavior (`hw/sim/usb_msc_sim.h` is the model).

### Overlaying custom userland utilities

`make image` automatically stages every custom NetBSD-hosted
utility into the image. Each utility's Makefile exposes an `overlay` target
that installs its files into a shared fake-root (`build/netbsd-overlay`,
`$(OVERLAY_ROOT)`); the image build then copies the whole tree into the
rootfs, preserving on-target paths and per-file modes (so non-binary data
like `terminfo.cdb` lands at the right place with the right mode).

```sh
make netbsd-overlay                    # build + stage all utilities (run automatically by image)
find build/netbsd-overlay -type f      # inspect exactly what will be installed
```

Staged into the tree: the demo/benchmark suite (`pbench`,
`mandelbrot`, `julia`, `plasma`, `lorenz`, `shadebobs`, `penumbra-text`) at
`/usr/local/bin/`, and **`penmon`** — the hardware-counter system monitor
(see `sw/penmon/README.md`). curses additionally needs the base-system
`terminfo.cdb`, which the full rootfs already includes; it is not overlaid,
to avoid colliding with the distribution's own copy.

**To add a new utility:**

1. Give its Makefile an `overlay` target that installs into `$(OVERLAY_ROOT)`,
   mirroring the on-target layout (e.g. `$(OVERLAY_ROOT)/usr/local/bin/foo`).
   Accept `OVERLAY_ROOT ?=` so it can be overridden by the top-level build.
2. In the top-level `Makefile`, add a `foo-overlay` passthrough target that
   invokes it, and append `foo-overlay` to `NETBSD_OVERLAYS`.

No changes to the image step are needed — the image build
absorbs whatever the overlay tree contains.

---

## 9. FPGA Synthesis (ULX3S)

FPGA synthesis uses the OSS CAD Suite (Yosys + nextpnr-ecp5 + ecppack + fujprog) via Docker. The wrappers in `hw/tools/oss-cad-suite/bin/` make them usable as normal commands:

```sh
export PATH="$PWD/hw/tools/oss-cad-suite/bin:$PATH"
```

### Build & flash

```sh
make fpga BOARD=ulx3s CORE=penumbra1   # Full flow: sv2v → yosys → nextpnr → ecppack
make fpga BOARD=ulx3s CORE=penumbra2   # Same flow for the gen2 pipelined core
make flash BOARD=ulx3s CORE=penumbra1  # Build + flash to ULX3S over USB (fujprog)
make fpga-lint                         # Verilator lint check on FPGA sources
```

Both cores are complete and bootable: `CORE=penumbra2` builds the 6-stage
pipelined core (the default), `CORE=penumbra1` the single-cycle microcoded
core. Each synthesizes to the ULX3S, closes timing at the 25 MHz CPU target,
and boots NetBSD to userland off the SD card. For a fast gen2 timing check during RTL
iteration, add `VARIANT=probe` to build the bare-core probe top — but read the
real fmax from the full `ulx3s_penumbra2_top` (the probe omits the cache/MMU/
arbiter layers and reports an optimistic number).

`BOARD`/`CORE` (plus optional `VARIANT`) expand to a registered top
module, `<board>_<core>[_<variant>]_top`, whose file lives under
`hw/rtl/fpga/<board>/`; an unknown combination is a hard error, and
`make fpga`/`flash`/`timing` with neither `BOARD`/`CORE` nor `TOP`
errors out (so you never silently build the wrong design).
`TOP=<module>` remains the low-level escape hatch for bare test tops.
The registry of valid combinations is `FPGA_TOPS` in the Makefile;
suite/axis semantics are specified in
`doc/internals/build-system.md`.

### Reading the build report

```sh
make timing BOARD=ulx3s CORE=penumbra1     # Pretty-print fmax + top critical paths
make timing BOARD=ulx3s CORE=penumbra1 TOP_N=10   # ...top 10 instead of 5
make fanout TOP=ulx3s_penumbra1_top        # High-fanout nets (router-congestion diagnosis)
make fanout TOP=ulx3s_penumbra1_top FANOUT_N=30 FANOUT_MIN=20
```

Both targets read whatever the last `make fpga` left in `build/`; they don't trigger a rebuild.

### SDRAM phase tuning

The SDRAM clock-pin phase is built into the bitstream filename via `PHASE_DEG`:

```sh
make fpga BOARD=ulx3s CORE=penumbra1 PHASE_DEG=180   # Default — step-4 baseline
```

Valid values: `0, 45, 90, 135, 180, 225, 270, 315`. See `doc/internals/sdram-controller.md` for the bring-up sweep procedure.

### Serial console

After flashing: `/dev/ttyUSB0` at **115200 8N1**. The boot ROM accepts `boot sd:0,0` to load `PENBOOT.ELF` from FAT32.
