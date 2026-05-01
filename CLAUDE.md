# Penumbra Minicomputer - Claude Code Context

## Project Overview
Penumbra is a 32-bit RISC-like minicomputer designed from scratch
and implemented on a Radiona ULX3S (Lattice ECP5) FPGA.
The project covers the full system: CPU, MMU, DMA, I/O, and system bus.
The eventual goal is to port NetBSD,
and later build the design from discrete 74xx chips.

## Key Decisions
- **HDL:** SystemVerilog for RTL
- **License:** BSD 2-Clause for Penumbra code; third-party code (LLVM, NetBSD) under original licenses. See LICENSE.
- **Toolchain:** Open-source FPGA tools (Yosys, nextpnr-ecp5, Project Trellis)
- **Simulation:** Verilator 5.046 via Docker (`verilator/verilator:latest`), driven by Makefile
- **Target board:** ULX3S with ECP5-85F (32 MB SDRAM, USB, HDMI, GPIO, etc.)
- **OS target:** NetBSD (drives privilege, interrupt, MMU design). See `doc/system/toolchain.md`
- **Compiler:** LLVM backend (GlobalISel, not SelectionDAG). ABI spec in `doc/system/abi.md`
- **Discrete build:** All design decisions must be feasible in 74xx discrete logic
- **Byte order:** Little-endian. `addr[1:0]=00` maps to bits `[7:0]`
  (defined by `byte_ext`/`byte_rep`). Matches x86/RISC-V/ARM-LE.
- **MUL/DIV/FP strategy:** Unified ALU, multi-cycle ops use alu_start/alu_busy.
  MUL/DIV initially trapped as illegal, SW emulated, hardware added incrementally.

## Architecture Summary
The architecture is fully specified in `doc/`. Key specs:
- **ISA:** `doc/system/architecture.md` —
  4-format 32-bit encoding (R/L/M/B), 2-operand, R0=zero,
  16 registers, ARM-style condition flags
- **Datapath:** `doc/internals/datapath.md` —
  three-bus (A/B/R), separate PC unit, 51-bit horizontal microcode,
  hardwired fetch unit, direct-mapped dispatch
- **Microcode reference:** `doc/internals/microcode.md` —
  complete micro-word format, field reference, ROM layout, sequencer behavior
- **Bus:** `doc/system/bus.md` (Software) and `doc/hardware/bus-protocol.md` (Hardware) —
  custom async Penumbra Bus (4-phase handshake), sync internal bus,
  sysreg sideband
- **MMU/Cache:** `doc/system/mmu.md` (Software) and `doc/internals/mmu-internals.md` (Hardware) —
  software-managed 64-entry 2-way SA TLB + 4-entry FA pinned TLB,
  split I/D PIPT cache,
  write-through D-cache
- **Sysregs:** `doc/system/sysregs.md` —
  WRSYS/RDSYS device map, register layouts, TLB packing
- **ABI:** `doc/system/abi.md` —
  ILP32, R1–R4 args, R5–R10 callee-saved, R11 scratch,
  R12 TP, R13 LR, R14 SP, R15 PC

## Repository Layout
- `hw/` - All hardware design (RTL, testbenches, microcode, boot ROM, tools).
  See `hw/CLAUDE.md` for detailed hardware context.
- `sw/` - Software tools and ISS (`sw/tools/` assembler/converter, `sw/sim/` instruction set simulator)
- `llvm/` - LLVM backend. See `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` for detailed LLVM context.
- `benchmark/` - Bare-metal benchmarks (Dhrystone). PIE ELFs loaded via
  `boot sd:0,0/DHRYSTON.ELF`. See "Benchmarks" section below.
- `doc/` - Architecture specs (System, Hardware, Internals)

## Conventions
- RTL filenames match the top-level module they contain
- One module per file
- Use `logic` rather than `reg`/`wire` where possible
- Prefix module ports: `i_` for inputs, `o_` for outputs
- Clock signal: `i_clk`, synchronous active-high reset: `i_rst` —
  sampled on rising edge of `i_clk`; while asserted, all state holds
  reset values; testbench holds for 2 cycles then releases
- Shared constants in `hw/rtl/core/penumbra_pkg.sv`
  (register addresses, ALU opcodes, condition codes)
- Modules that use the package: `import penumbra_pkg::*;` inside the
  module declaration (not at file scope —
  Verilator warns about `import *` at $unit scope)

### Naming: Hardware vs Software Terminology
- **Supervisor** = hardware privilege level (SR.S bit, CPU mode). Use for CPU-level concepts.
- **Kernel** = OS software running in supervisor mode. Use for OS-level concepts.
- **Special-purpose registers (SPRs)** = CPU-internal registers via
  `RDSPR`/`WRSPR` (ESR, EPC, USP, SR). SPR number encoded in IR[15:12].
- **System registers (sysregs)** = device-mapped registers via
  `WRSYS`/`RDSYS` (MMU, TLB, system ID). Belong to peripheral devices.

### Commit Messages
Use `<subsystem>: <description>` (lowercase subsystem, colon, short
imperative description). Examples:
- `llvm: fix PIC TLS GD materialization`
- `netbsd: bump UPAGES from 3 to 4`
- `rom: fix detect_page readback across bus-faulted probes`
- `doc: describe the pinned vector page`
- `test: catch test_sysid up with CPU_FREQ sysreg`

Common subsystem tags: `llvm`, `netbsd`, `rom`, `hw`, `sw`, `doc`,
`test`, `benchmark`. Keep the subject under ~70 chars; put detail
in the body. Don't use bracketed tags (`[LLVM]`) — that's a legacy
style from earlier in the project.

## Build System
- `make simulate` — build boot ROM + ISS, run interactively (fast,
  no Docker). Uses instruction-level simulator (`sw/sim/penumbra_iss.cpp`).
  - Override LLVM location: `make simulate LLVM_PREFIX=/path/to/llvm-build`
  - With SD card image: `make simulate SDCARD=disk.img`
  - Instruction trace: `make simulate TRACE=build/trace.log`
    (dumps PC, SR, R1–R14 for every instruction)
  - Raw TTY mode: `make simulate RAW=1` passes all control characters
    (Ctrl-C, Ctrl-Z, etc.) through to the guest OS for job control.
    Use Ctrl-A as escape prefix (Ctrl-A X = exit, Ctrl-A C = CPU state,
    Ctrl-A H = help, Ctrl-A Ctrl-A = literal Ctrl-A).
  - ROM monitor accepts `break` (or `b`) to halt the simulator cleanly.
- `make simulate-rtl` — build boot ROM + Verilator RTL sim,
  run interactively via Docker (`-it`). Cycle-accurate but slow.
  - Non-interactive (piped input): `echo "break" | make simulate-rtl INTERACTIVE=0`
  - Same SDCARD/TRACE options as `make simulate`.
- `make test-iss` — run all `hw/sim/programs/test_*.s` on ISS
  (fast, no Docker); reports pass/fail summary
- `make test` — run all test programs on RTL sim via Docker
- `make test-compiler` — run comprehensive C compiler correctness
  tests from `llvm-test-suite` on ISS in `+hosted` mode.
  See "Compiler Correctness Tests" section below for setup.
- `make smoke` — toolchain smoke test (trivial adder)
- `make sim MOD=<name>` — build & run a module's Verilator testbench
- `make sim MOD=machine_sim TB=<tb> PROG=<prog>` —
  run specific testbench with specific program.
  Auto-assembles `.s`/`.uasm` into hex.
- `make wave MOD=<name>` — open VCD waveform in GTKWave
- `make clean` — remove build artifacts
- RTL simulation runs via Docker — no host install needed.
  ISS compiles natively with g++ (no dependencies).
  Build artifacts in `build/` (gitignored).
- **Important:** `rm -rf build/<mod>.verilator build/V<mod>`
  if you suspect stale binaries (WSL2 stale mtimes)

### FPGA Toolchain (OSS CAD Suite)
FPGA synthesis uses Yosys, nextpnr-ecp5, ecppack, and fujprog via
Docker.  Wrapper scripts in `hw/tools/oss-cad-suite/bin/` make them
usable as normal commands:
`export PATH="$PWD/hw/tools/oss-cad-suite/bin:$PATH"`.

- **FPGA build flow:**
  `make fpga TOP=ulx3s_top` — full build (sv2v → fix → yosys → nextpnr → ecppack)
  `make flash TOP=ulx3s_top` — build + flash to ULX3S via USB
  `make fpga-lint TOP=ulx3s_top` — Verilator lint check
  `make timing TOP=ulx3s_top [TOP_N=10]` — pretty-print fmax + top critical
  paths from the last `make fpga` run (reads `build/<top>_timing.json`)
- **ULX3S system:** `hw/rtl/fpga/ulx3s_top.sv` — board top-level,
  12.5 MHz PLL (25 MHz crystal), 32 MB SDRAM (W9825G6KH or compatible),
  real UART (TX+RX), real SPI with SD card (autoconfig), boot ROM,
  btn[1] reset.  Serial: 115200 8N1 on `/dev/ttyUSB0`.
  SD card: micro SD slot in SPI mode, autoconfigured as CLASS\_SD.
  Boot ROM `boot sd:0,0` loads `PENBOOT.ELF` from FAT32 partition.
  SDRAM controller: CL=2, BL=2, auto-precharge, universal-safe timings
  for all ULX3S SDRAM variants (see `doc/internals/sdram-optimization.md`).

See `hw/CLAUDE.md` for toolchain mechanics (wrapper multi-call
pattern, sv2v + `$readmemh` workaround, interactive shell,
USB passthrough, adding new tools).

### SD Card Image
Build SD images for `make simulate SDCARD=build/boot.img`:
```sh
make sdimage              # boot partition only (FAT32: bootloader + kernel)
make sdimage-rootfs       # boot + FFS root (minimal rescue, ~86 MB)
make sdimage-rootfs ROOTFS_FULL=1  # boot + FFS root (full distribution)
```
The rootfs script (`sw/tools/mkrootfs.sh`) creates an FFS image from
`build/netbsd-dest/`.  Minimal mode (`-m`) includes only `/rescue`
(statically linked, works without `ld.elf_so`), `/lib`, and `/etc`.
The SD image has two MBR partitions: FAT32 boot (first) and
FFS root (second, mounted as `ld0f` in the kernel).  Rootfs
images include a `boot.cfg` on the FAT32 partition with
`root=ld0f`, so `boot sd:0,0` reaches single-user shell with
no further interaction.  The rootfs also includes an `/etc/fstab`
mapping `/` to `/dev/ld0f` (so `mount -uw /` works without
arguments) and `/dev/ld0*` nodes via `MAKEDEV -s std ld0`.
Requires NetBSD cross-tools (`nbfdisk`, `nbmakefs`).

### LLVM Toolchain Build
Build dir: `build/llvm/` (override with `LLVM_PREFIX`).
Initial cmake and incremental `ninja` invocations are documented in
`llvm/llvm/lib/Target/Penumbra/CLAUDE.md`.  Always target only what
the project needs (`llc clang lld llvm-mc llvm-ar …`) — a bare
`ninja -C build/llvm` also builds all upstream unit tests.

### LLVM Backend Tests
Regression tests: `llvm/llvm/test/CodeGen/Penumbra/` (Lit + FileCheck).
```sh
build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/       # all
build/llvm/bin/llvm-lit -v llvm/llvm/test/CodeGen/Penumbra/alu.ll  # one
```
Regenerate CHECK lines with `update_llc_test_checks.py` after codegen
changes (see llvm subtree CLAUDE.md for the full command).

### Compiler Correctness Tests
Comprehensive C tests from `llvm-test-suite` (including GCC torture)
running on ISS in `+hosted` mode. Requires `compiler-rt`.  Current
state: 1606/1606 passing at both `-O0` and `-O2`; excluded tests
(harness-limitation or upstream-known-bad) and deferred backend
gaps are tracked in `test/compiler/excludes.txt` and `doc/TODO.md`.

Build `compiler-rt` builtins (one-time): `sw/tools/setup-compiler-rt.sh`.

Run tests:
```sh
make test-compiler                # all tests at -O2
make test-compiler OPT="-Os"      # override optimization
make test-compiler COMPILER_TESTS="path/to/test.c"  # single test
```
Full report in `build/test-compiler-report.txt`.

### NetBSD Kernel Build
Prerequisites: NetBSD tools built via `build.sh` (one-time).
Build output: `build/netbsd-kernel/MINIMAL/` (out of source tree).
Full command sequence (`nbconfig`, `nbmake-penumbra depend`,
`nbmake-penumbra`) in `netbsd/sys/arch/penumbra/CLAUDE.md`.

### Boot ROM Build Pipeline (`make simulate`)
The boot ROM has its own Makefile (`hw/rom/Makefile`) with automatic
source discovery and header dependency tracking.  The main Makefile
delegates with `$(MAKE) -C hw/rom`; can also be built standalone.
```
hw/rom/*.c    →  clang -c  →  *.o  ─┐
hw/rom/*.s    →  llvm-mc   →  *.o   ├→ ld.lld (rom.ld) → boot_rom.elf → objcopy → bin2hex → program.hex
hw/rom/rom.ld ─────────────────────────┘
```

## Benchmarks
Bare-metal benchmarks in `benchmark/`, built as PIE ELFs loaded from
SD card FAT32 via `boot sd:0,0/DHRYSTON.ELF`.

**Harness** (`benchmark/common/`): PIE self-relocating CRT, identity-mapping
TLB miss handler (cached RAM, uncached MMIO), timer IRQ for tick
accumulation, polled UART output, software mul/div (librt.c).
Compiler runtime and string functions provided (no compiler-rt/libc
dependency).  Soft-float stubs for benchmarks that use float in
reporting (not in measurement loops).
Both caches are enabled in `crt0.S` after MMU bring-up (cache hardware
is disabled at reset; `PTE.C` declares cacheability per page but the
master `CACHE_CTRL.ENABLE` must also be set for any caching to occur).
Per-bench overrides via `bench_caches_disable()`/`bench_caches_enable()`
in `bench.h`.  Bench-side perfctr snapshot helpers
(`bench_perf_snapshot()`/`bench_perf_print_delta()`) read the
`SYSDEV_CPU` cycles/insns_retired counters and print CPI alongside DMIPS.

**Dhrystone 2.1** (`benchmark/dhrystone/`): Original 1988 source files
(dhry.h, dhry_1.c, dhry_2.c) unmodified from Reinhold P. Weicker's
Usenet posting.  Adapted via shim headers (`-isystem dhrystone/include`)
and `-Dmain=dhrystone_main`.

**Memtest** (`benchmark/memtest/`): Memory correctness tester. Walks
an 8 MiB window of SDRAM (configurable via `MEMTEST_MAX_BYTES`)
with six patterns — round-trip / walking-1 / walking-0 / complement /
sub-word / address-as-data — each catching a distinct failure class
(stuck-at bits, address-line swaps, DQM mask, byte-en pipeline,
write-disturb).  Loaded as `MEMTEST.ELF`.  Long patterns emit
liveness dots (~1 Hz on hardware) so a slow run is visibly distinct
from a hang.  Calls `bench_caches_disable()` at entry: every access
must round-trip through SDRAM or write-disturb errors get hidden by
cached repeat-reads.

**Membench** (`benchmark/membench/`): Memory throughput / latency
baseline.  Cached sweep of a 64 KiB working set (defeats the 1 KiB
cache 64×, exercises burst-fill on every line miss) and uncached
single-page latency on a `PTE_KERNEL_NC`-pinned page, measured at
W/H/B sizes for both R and W.  All inner loops are hand-unrolled 8×
so the per-access loop overhead doesn't drown the actual cache /
SDRAM cost — needed because volatile loads/stores prevent clang
from auto-unrolling.  Loaded as `MEMBENCH.ELF`.  Reports MB/s
(cached, fractional to 3 decimals) and ns/op (uncached).  Designed
to expose the impact of step 6's open-row optimization
(`doc/internals/sdram-controller.md`).

Build and run:
```sh
make benchmark                    # ISS (fast, ~seconds)
make benchmark-rtl                # Verilator (cycle-accurate, slower)
make benchmark BENCH_ITERS=100    # override iteration count
make benchmark COPT="-Os"         # override optimization level
```

Baseline results (Dhrystone 2.1, -O2, 10000 iters, no hardware MUL,
caches enabled, 12.5 MHz CPU clock):
- ULX3S FPGA: 0.88 DMIPS (CPI ≈ 7.10, ~1555 Dhrystones/sec)

CPI sits well above the raw microcode floor (~3.8) because the 1 KiB
direct-mapped caches thrash on Dhrystone's working set, paying
SDRAM-refill cost through the CDC bridge on every miss.  Smaller
contributions from software muldiv (Proc_8 array indexing) and
taken-branch refill bubbles.  Adding fetch_cycles / stall_cycles
perfctrs is the next step toward attributing the gap precisely.

## Current Status
The CPU is fully functional in simulation: all RTL modules implemented
and tested, CPU runs real programs through the full
CPU → MMU → split I/D cache → memory path,
booting from ROM at `0xFFFF_0000`.
Eight exception sources (IRQ, MMU faults, alignment, bus fault,
BREAK, SYSCALL, privilege, illegal) are fully wired with
MIPS/68k-style vector dispatch.

**Bus autoconfig and SD card boot path working end-to-end.**
- Boot ROM runs bus autoconfig: resets the bus, enables the config
  chain, probes devices via bus-fault detection, and allocates base
  addresses.  See `doc/system/bus.md` for the protocol.
- First autoconfigured device is the SPI controller (`CLASS_SD`,
  assigned `0xFF001000`).  SPI v2 has a 7-register interface with
  a 16550-style FIFO enable bit — polled single-byte for ROM,
  FIFO burst for kernel.  See `doc/system/devices/spi.md`.
- **ROM FAT32 boot:** `boot sd:<dev>,<cs>[/file]` mounts the first
  FAT32 partition and loads the named file (default `PENBOOT.ELF`)
  as a PIE ELF.  The ROM parses ELF headers, allocates RAM via
  `find_memory_region()`, copies PT_LOAD segments, and jumps to
  the entry point with R1=bootdata.  No hardcoded load address.
  Tested end-to-end with `nbmakefs`-generated images.
- SD naming uses per-class controller index (`sd:0,0` = first SD
  controller, CS0), not the global device index.
- See `hw/CLAUDE.md` for RTL module details (busctl, autoconfig_dev,
  sim_spi, SD card emulator) and the ROM monitor command reference.

**LLVM toolchain is end-to-end functional.**
- Clang/lld/llvm-mc cover the full pipeline: C → object → ELF.
  Boot ROM, kernel, and full NetBSD userland all compile and link.
- GlobalISel codegen (no SelectionDAG), MC-layer assembler and
  disassembler, 22 relocation types, PIE/GOT/PLT, TLS (GD + LE),
  soft-float, atomics via `__atomic_*` libcalls, varargs, VLAs,
  inline asm, C++ EH with DWARF unwinding and libunwind.
- Branch analysis enables `-O0` through `-O2`.
- Target triples: `penumbra-unknown-netbsd` (userland/kernel),
  `penumbra-unknown-none` (bare-metal ROM/hw).
- See `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` for backend details
  (file map, legalization, codegen coverage, relocation table,
  TableGen/C++ split, PIC/TLS implementation notes).

## Software Tools
- **Instruction Set Simulator** (`sw/sim/penumbra_iss.cpp`):
  Fast instruction-level simulator for software development.
  Single C++ file, no dependencies beyond g++.
  Covers full ISA (all 4 formats), 8 exception types,
  software-managed TLB (64-entry 2-way SA + 4-entry FA pinned), privilege modes
  with SP banking, 16450 UART, SPI+SD card emulation,
  and bus autoconfig. Passes all 38 hardware test programs.
  Build: `make -C sw/sim` (or built automatically by `make simulate`).
  Run: `sw/sim/penumbra-iss program.hex [+sdcard=img] [+trace=log]
  [+raw] [+trap-pc0]`.  `+trap-pc0` aborts the sim with full register
  dump if user-mode code ever fetches from PC=0 — useful for catching
  null-pointer jumps (bad function pointers, clobbered LR on return).
  Off by default because some OS-level tests may deliberately probe
  memory protection at address 0.
- **LLVM toolchain** (`build/llvm/bin/`, override with `LLVM_PREFIX`):
  clang (C compiler), llvm-mc (assembler), ld.lld (linker),
  llvm-objcopy. Target triple: `penumbra-unknown-netbsd` (userland/kernel),
  `penumbra-unknown-none` (bare-metal ROM/hw).
  See `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` for backend details.
- **Microcode assembler** (`hw/tools/uasm.py`):
  Symbolic microcode → $readmemh hex.
  Run: `python3 hw/tools/uasm.py input.uasm -o microcode.hex`
- **ISA assembler** (`sw/tools/pasm.py`):
  Two-pass assembler, all 4 formats, labels,
  pseudo-ops (NOP, RET, LA, LI), `.equ`, data directives.
  Comments: both `;` (legacy) and `//` (LLVM convention).
  Still used by `make test` for hardware test programs.
  Run: `python3 sw/tools/pasm.py --org 0xFFFF0000 input.s -o program.hex`
- **Binary-to-hex converter** (`sw/tools/bin2hex.py`):
  Flat LE binary → $readmemh hex. Used in the clang pipeline.
- Hex files are gitignored. Makefile auto-builds them from sources.

## Test Convention
- **Program runner** (`hw/sim/tb_cpu_prog.cpp`):
  Runs program until BREAK (500000 cycle limit),
  checks R1 for pass/fail.
- **Pass/fail:** R1 = 1 means PASS, R1 = 0 means FAIL.
  Tests self-check and set R1.
- **Halt:** Testbench watches `o_halted` pulse (BREAK dispatch). Programs end with `BREAK`.
- **Boot from ROM:** Programs assembled with `--org 0xFFFF0000`. `_start:` must be first label.
- **ROM page mapping (MMU tests):** TLB_INDEX=16, TLB_VPN=0x0FFFF000, TLB_PTE=0xFFFF00B9.

**NetBSD kernel mounts root filesystem from SD card on the ISS.**
- Kernel builds as a ~5 MB ELF at `build/netbsd-kernel/MINIMAL/netbsd`
  (DIAGNOSTIC enabled).
- Full MD layer: real pmap (2-level page tables, generational ASID,
  PV lists), page-table-walking TLB miss handler, shared-IRQ
  dispatch (no intr controller — wire-OR with `LIST_HEAD` walk),
  context switching, demand paging via `uvm_fault()`,
  copyin/copyout with `pcb_onfault` recovery, syscall dispatch
  (carry-flag convention, R11=nr, R1–R4 args), signal delivery
  (siginfo + ucontext), TLS (R12=TP), RAS-based userland atomics.
- MI drivers attached via `pbbus` bridge: 16550 UART
  (`com(4)` IRQ-driven, system console), SD/MMC host controller
  (`pmci` → `sdmmc` → `ld_sdmmc` → `ld0`).
- `/rescue/init` reaches interactive single-user shell with FFS
  root mounted rw on `ld0f`.  Read and write both verified.
- VM layout: 2G/2G user/kernel split, kernel text at `0x8001_0000`,
  top 5 pages reserved for pinned TLB slots (vector, L1, L2 window,
  scratch, guard).  VA 0 unmapped (null guard).
- Remaining MD stubs: `process_read_regs`, `process_write_regs`,
  `process_set_pc`, `cpu_coredump`, `vmapbuf`/`vunmapbuf`
  (grep `TODO(stub)`).  DDB disabled.
- See `netbsd/sys/arch/penumbra/CLAUDE.md` for detailed kernel
  port context (directory layout, file map, VM layout details,
  pinned-slot naming, status checklist).

**NetBSD userland cross-build fully functional.**
- `build.sh distribution` completes: all libraries, all programs,
  and system configuration (`etc.penumbra`).  Uses stock
  `toolchains::NetBSD` with Penumbra emulation/flags in
  `NetBSD.cpp` (no custom toolchain class for NetBSD —
  `PenumbraToolChain` retained for bare-metal only).
- Penumbra-specific files live under `netbsd/{lib,common,libexec}/**/arch/penumbra/`:
  CSU (`crt0.S`/`crti.S`/`crtn.S`/`crtbegin.h`/`crtend.S`),
  libc MD (`SYS.h`, `cerror.S`, syscall wrappers, `genassym.cf`,
  `swapcontext.S`, `_lwp_makecontext`), machine headers
  (`asm.h`, `fenv.h`, `ieee.h`, `setjmp.h`, `mcontext.h`, …).
- C++ enabled (`MKCXX=yes`): libunwind (`Registers_penumbra` + save/
  restore assembly) built into libc; libc++ and libcxxrt link as
  shared libraries; libatf-c available for the ATF test suite.
- **Dynamic linker (`ld.elf_so`) functional**: `rtld_start.S` +
  `mdreloc.c` in `libexec/ld.elf_so/arch/penumbra/`.  Self-relocation
  via RELA (needs `--apply-dynamic-relocs` at link time).
  TLS Variant I with `__HAVE___LWP_GETTCB_FAST`; common
  `__tls_get_addr`.  Dynamically-linked binaries (`/bin/sh`,
  `/bin/ls`, `ldd`) load and run end-to-end on the ISS.
- Known shortcuts (libpthread minimal stubs, etc.) tracked in the
  `project_userland_shortcuts.md` memory file.

## Next Steps (in priority order)
1. **ATF regression tests** — build rootfs with test suite, run
   `t_swapcontext` and other ATF tests on the ISS.
2. **Root filesystem** — `build.sh sets` to create installable sets,
   boot with full userland on the ISS.
3. **Memory subsystem** — SDRAM controller, bus interface.

Kernel-side follow-ups (remaining MD stubs, SPI FIFO + IRQ-driven
pmci) are tracked in `netbsd/sys/arch/penumbra/CLAUDE.md`.
