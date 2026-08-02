# Penumbra Minicomputer — Claude Code Context

Penumbra is a 32-bit RISC-like minicomputer designed from scratch and
implemented on a Radiona ULX3S (Lattice ECP5) FPGA — CPU, MMU, DMA, I/O,
and system bus. It boots NetBSD to userland on the FPGA today; the
remaining long-term goal is to re-implement the gen1 design (and the ISA
it proves) in discrete 74xx logic.

## Where to find things

**Architectural specifications live in `doc/`.** The audience-organized
index is [`doc/README.md`](doc/README.md). Always read those first
before assuming an architectural fact — they are the source of truth,
and CLAUDE.md files intentionally do *not* duplicate them.

| Topic | Read |
|-------|------|
| ISA, registers, encoding, instruction set | `doc/system/{architecture,instruction-set,instruction-encoding}.md` |
| ABI, ELF relocations, calling convention | `doc/system/abi.md` |
| MMU, sysregs, bus address map, boot protocol | `doc/system/{mmu,sysregs,bus,boot-protocol}.md` |
| Device register maps (SPI/SD, ESP32 NIC) | `doc/system/devices/` |
| NetBSD port — current status, kernel design | `doc/system/netbsd/{porting-status,design-notes}.md` |
| Bus signal-level protocol (hardware) | `doc/hardware/bus-protocol.md` |
| Penumbra/1 datapath, microcode, L1 cache | `doc/internals/penumbra1/` |
| MMU internals, L2 cache, CPU-internal bus | `doc/internals/{mmu-internals,l2-cache,cpu-bus}.md` |
| Penumbra/2 (gen2) pipelined-core design | `doc/internals/penumbra2/` |
| SDRAM controller / optimization | `doc/internals/sdram-{controller,optimization}.md` |
| RTL coding standards | `doc/internals/coding-standards.md` |
| Current TODO / roadmap | `doc/TODO.md` |

**Subsystem CLAUDE.md files** add navigation hints (file maps, key
gotchas) for working inside a subtree:
- `hw/CLAUDE.md` — RTL module map, boot-ROM file layout, FPGA toolchain
- `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` — backend file map, build
- `netbsd/sys/arch/penumbra/CLAUDE.md` — MD source map, kernel build

## Repository layout

- `hw/` — RTL, microcode, boot ROM, testbenches, FPGA tools
- `sw/` — `sw/sim/` ISS, `sw/tools/` assembler/converters/scripts
- `llvm/` — LLVM backend (in-tree fork)
- `netbsd/` — NetBSD source subtree, MD code at `sys/arch/penumbra/`
- `benchmark/` — Bare-metal + NetBSD-hosted benchmarks
- `doc/` — Architectural specs (start at `doc/README.md`)

## Key cross-cutting decisions

- **HDL:** SystemVerilog 2012. **License:** BSD 2-Clause (third-party
  code under original licenses; see `LICENSE`).
- **Compiler:** in-tree LLVM, GlobalISel (not SelectionDAG). Triples:
  `penumbra-unknown-netbsd` (kernel + userland), `penumbra-unknown-none`
  (bare-metal ROM and hw tests).
- **OS target:** NetBSD — drives privilege, interrupt, and MMU design.
- **Byte order:** little-endian. `addr[1:0]=00` maps to bits `[7:0]`.
- **Discrete-logic constraint (ISA + gen1 only):** the ISA design and
  **gen1 (penumbra1)** must be feasible in 74xx discrete logic — no
  FPGA-specific tricks the chip-level rebuild could not match; there,
  favor mux-/register-/bus-minimal options. **gen2 / gen2.5 / gen3 are
  FPGA-only** (pipelined, BRAM caches, wide bypass) and will *not* be
  rebuilt in discrete logic, so never weigh their microarchitecture by
  74xx feasibility or chip count. What every generation still honors is
  the externally-visible ISA contracts (bus protocol, autoconfig, device
  sysreg interface) — those are ISA-level, not microarchitecture.
- **MUL/DIV/FP:** MUL/DIV execute in hardware on the `divmul` peer unit
  (own `start`/`busy` handshake, ~33-cycle iteration); divide-by-zero is
  detected in hardware and raises `VEC_ARITH`. FP is software-only.

## Conventions

### RTL
- One module per file; filename matches the top-level module name.
- Use `logic`, not `reg`/`wire`.
- Port prefixes: `i_` input, `o_` output. Clock `i_clk`, synchronous
  active-high reset `i_rst` (held 2 cycles by testbenches).
- `import penumbra_pkg::*;` *inside* the module declaration, not at
  file scope (Verilator warns about `$unit`-scope wildcard imports).
- Shared constants live in `hw/rtl/common/penumbra_pkg.sv`.
- Registered signals end in `_q`, their combinational next-state in `_d`
  (`x_q <= x_d`); plain names are combinational — registered-vs-combinational
  must be visible at every use site.
- Name signals by semantic meaning (`i_first_cycle`, not `i_fresh`).
- Comments explain *why*, never restate *what*, and are non-temporal (no
  "changed from…"/history — git holds that).
- Module instances: one parameter and one port per line (minimizes diff churn).
- Full RTL style guide: `doc/internals/coding-standards.md`.

### Naming: hardware vs software terminology
- **Supervisor** = hardware privilege level (SR.S bit). Use for
  CPU-level concepts.
- **Kernel** = OS software running in supervisor mode. Use for
  OS-level concepts.
- **Special-purpose register (SPR)** = CPU-internal register accessed
  via `RDSPR`/`WRSPR` (ESR, EPC, USP, SR). SPR number in IR[15:12].
- **System register (sysreg)** = device-mapped register accessed via
  `RDSYS`/`WRSYS` (MMU, TLB, system ID). Belongs to a peripheral.

### Commit messages
Format: `<subsystem>: <imperative description>` (lowercase subsystem,
subject under ~70 chars; detail in the body). Common subsystem tags:
`llvm`, `netbsd`, `rom`, `hw`, `sw`, `doc`, `test`, `benchmark`.
Examples: `llvm: fix PIC TLS GD materialization`,
`netbsd: bump UPAGES from 3 to 4`. Do not use bracketed tags
(`[LLVM]`) — that's a legacy style.

## Build System

### Simulation and tests
- `make simulate` — boot ROM + ISS, interactive (fast, no Docker).
  Options: `LLVM_PREFIX=`, `SDCARD=disk.img`, `USBDISK=disk.img`
  (attach a USB mass-storage device), `TRACE=trace.log`,
  `RAW=1` (passes Ctrl-C etc. to guest; Ctrl-A is escape prefix —
  `Ctrl-A X` exit, `Ctrl-A C` CPU state, `Ctrl-A H` help).
  ROM monitor accepts `break` (or `b`) to halt cleanly.
- `make simulate-rtl` — boot ROM + Verilator RTL sim, interactive via
  Docker (`-it`). Cycle-accurate but slow. `INTERACTIVE=0` for piped
  input. Same `SDCARD`/`TRACE` options.
- `make test-iss` — the `hw/sim/programs/isa/` conformance suite on
  the ISS (no Docker).
- `make test [CORE=penumbra1|penumbra2]` — `isa/` + the core's
  microarch suite (`hw/sim/programs/<core>/`) on RTL sim via Docker.
  `CORE` also takes a microarch sub-variant (`penumbra<n>_<sub>`, e.g.
  `penumbra2_5`): the base generation built with one core parameter set,
  inheriting its suite (mechanism in `doc/internals/build-system.md`).
  `make test-prog CORE=<core> PROG=<name>` runs one program. Programs
  carry lit-style `; RUNNER:` / `; REQUIRES:` header tags (scanned by
  `hw/tools/run-prog-tests.py`); suite layout and tag semantics in
  `doc/internals/build-system.md`.
- `make test-modules` — module-level Verilator testbenches (alu,
  regfile, cache_test, sdram_adapter_test, …). List is `MODULE_TESTS`
  in the Makefile; integration testbenches (`tb_cpu_prog`,
  `tb_interactive`) are intentionally excluded. Add new entries when
  introducing modules with a `tb_<mod>.cpp` or `<mod>_test.sv` wrapper.
- `make test-all` — `make test` then `make test-modules`.
- `make test-compiler` — `llvm-test-suite` C tests on ISS in `+hosted`
  mode. Requires `compiler-rt` built per optimization level
  (`sw/tools/setup-compiler-rt.sh` builds `-O0` and `-O2` archives;
  the suite links the one matching `OPT`). Override `OPT=` or
  `COMPILER_TESTS=path/to/test.c`. Report in
  `build/test-compiler-report.txt`. Excludes and deferred backend gaps
  in `test/compiler/excludes.txt` and `doc/TODO.md`.
- `make sim MOD=<name>` — module's Verilator testbench.
  `make sim MOD=machine_sim TB=<tb> PROG=<prog>` — runs a specific
  testbench with a specific program (auto-assembles `.s`/`.uasm`).
- `make wave MOD=<name>` — open VCD in GTKWave.

**Simulation gotchas:**
- **Dual-clock sim.** `machine_sim`'s SDRAM subsystem runs on a
  separate `i_sdram_clk` driven at 4× the CPU clock by both
  `tb_interactive.cpp` and `tb_cpu_prog.cpp`, matching the ULX3S
  25 MHz / 100 MHz hardware ratio. Tying `i_sdram_clk` to `i_clk` is
  still a valid same-rate config if a testbench needs it.
- **`OPT_BUILD` knob** controls host C++ optimization of the
  Verilator-generated code. Default `-O2`. For long-running sims
  (NetBSD boot): `OPT_BUILD="-O3 -flto -march=native"` (~3× slower
  build, ~2× faster runtime). For testbench iteration: `OPT_BUILD="-Os"`
  (Verilator's default — faster rebuilds).
- **Stale Verilator binaries on WSL2.** If results look wrong after
  edits, `rm -rf build/<mod>.verilator build/V<mod>`. Required when
  changing `OPT_BUILD` (ccache won't detect the flag change).

### FPGA synthesis (OSS CAD Suite via Docker)
Wrapper scripts in `hw/tools/oss-cad-suite/bin/` make Yosys,
nextpnr-ecp5, ecppack, fujprog usable as normal commands:
`export PATH="$PWD/hw/tools/oss-cad-suite/bin:$PATH"`.

- `make fpga BOARD=ulx3s CORE=penumbra1` — sv2v → fix → yosys →
  nextpnr → ecppack. `BOARD`/`CORE` (+ optional `VARIANT`) expand to a
  registered top module (`<board>_<core>[_<variant>]_top`, file under
  `hw/rtl/fpga/<board>/`); an unknown combination is a hard error.
  A microarch sub-variant `CORE` (e.g. `penumbra2_5`) reuses its base
  board top parameterized — `TOP` names the per-variant artifact,
  `TOP_MODULE` the shared synth module (`doc/internals/build-system.md`).
  `TOP=<module>` stays as the low-level escape hatch.
- `make flash BOARD=ulx3s CORE=penumbra1` — build + flash via USB.
- `make fpga-lint` — Verilator lint check (full gen1 system).
- `make timing BOARD=ulx3s CORE=penumbra1 [TOP_N=10]` — pretty-print
  fmax + top critical paths from `build/<top>_timing.json`.
- gen2 timing checks: `make fpga`/`timing` with
  `BOARD=ulx3s CORE=penumbra2 VARIANT=probe` builds the bare-core
  probe top — run after gen2 RTL changes to catch fmax movement early.

Board top-levels under `hw/rtl/fpga/ulx3s/`: `ulx3s_penumbra2_top.sv`
(the default core) and `ulx3s_penumbra1_top.sv`. Serial 115200 8N1 on
`/dev/ttyUSB0`. SD slot autoconfigured as `CLASS_SD`; `boot sd:0,0`
loads `PENBOOT.ELF` from FAT32. Toolchain mechanics (wrapper
multi-call, sv2v + `$readmemh` workaround, USB passthrough) in
`hw/CLAUDE.md`.

### LLVM build
Build dir: `build/llvm/` (override with `LLVM_PREFIX`). Initial cmake
and incremental `ninja` invocations are in
`llvm/llvm/lib/Target/Penumbra/CLAUDE.md`. Always target only what's
needed (`llc clang lld llvm-mc llvm-ar …`); a bare `ninja -C build/llvm`
also builds upstream unit tests.

Backend regression tests:
```sh
build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/       # all
build/llvm/bin/llvm-lit -v llvm/llvm/test/CodeGen/Penumbra/alu.ll  # one
```
Regenerate CHECK lines with `update_llc_test_checks.py` (full command
in the LLVM subtree CLAUDE.md).

### NetBSD kernel and SD images
- Kernel build: `build.sh kernel=MINIMAL` from `netbsd/`; output lands
  at `build/netbsd-obj/sys/arch/penumbra/compile/MINIMAL/netbsd`. Full
  sequence in `DEVELOP.md` § 7.
- SD images: `make sdimage` (boot only), `make sdimage-rootfs`
  (boot + full FFS distribution). Boot-only images need a kernel at
  `PENBOOT.ELF`; rootfs images include `boot.cfg` with `root=ld0f` so
  `boot sd:0,0` reaches single-user with no prompts. `sdimage-rootfs`
  also drops the bare-metal benchmark ELFs onto the FAT32 boot
  partition, so `boot sd:0,0/DHRYSTON.ELF` runs a benchmark from the
  same card. Requires NetBSD cross-tools (`nbfdisk`, `nbmakefs`) built
  once via `build.sh tools`. `sdimage-rootfs` auto-overlays the custom
  userland utilities (benchmark suite + `penmon`) into `/usr/local/bin`
  via each tool's `overlay` make target staged into
  `build/netbsd-overlay`, copied in with `mkrootfs.sh -O`; see
  DEVELOP.md § 8 for the convention and how to add a utility.

### Boot ROM build
The ROM has its own `hw/rom/Makefile` (auto source discovery + header
deps). Main Makefile delegates via `$(MAKE) -C hw/rom`. Pipeline:
```
*.c → clang -c → *.o  ─┐
*.s → llvm-mc  → *.o  ├→ ld.lld (rom.ld) → boot_rom.elf → objcopy → bin2hex → program.hex
rom.ld ──────────────────┘
```

## Software tools

- **ISS** (`sw/sim/penumbra_iss.cpp`) — fast instruction-level
  simulator, single C++ file. Covers full ISA + 8 exceptions +
  software-managed TLB + UART + SPI/SD + bus autoconfig. Run:
  `sw/sim/penumbra-iss program.hex [+sdcard=img] [+usbdisk=img]
  [+trace=log] [+raw] [+trap-pc0]`. `+trap-pc0` aborts on PC=0 fetch (catches null-pointer
  jumps; off by default because some OS tests probe address 0).
- **LLVM toolchain** (`build/llvm/bin/`) — clang, llvm-mc, ld.lld,
  llvm-objcopy. Details in
  `llvm/llvm/lib/Target/Penumbra/CLAUDE.md`.
- **Microcode assembler** (`hw/tools/uasm.py`) — symbolic microcode
  → `$readmemh` hex.
- **ISA assembler** (`sw/tools/pasm.py`) — two-pass, all 4 formats,
  labels, pseudo-ops, `.equ`, data directives. Still used by
  `make test` for hardware test programs. Comments: `;` (legacy)
  or `//` (LLVM convention).
- **bin2hex** (`sw/tools/bin2hex.py`) — flat LE binary → `$readmemh`
  hex. Used in the clang pipeline.

Hex files are gitignored; the Makefile builds them from sources.

## Test convention

- **Suites:** `hw/sim/programs/isa/` is the conformance suite (must
  pass on the ISS and every core generation); `hw/sim/programs/<core>/`
  holds microarch-pinned regressions. Tag semantics and layout:
  `doc/internals/build-system.md`.
- **Runners:** gen1 `hw/sim/tb_cpu_prog.cpp` (runs until BREAK halts,
  500000 cycle limit); gen2 `hw/sim/tb_penumbra2_prog.cpp` (stops on a
  retiring BREAK — gen2 traps rather than halts). Both check R1.
  A `; RUNNER: tb_<name>` header tag selects a bespoke testbench.
- **Convention:** R1 = 1 means PASS, R1 = 0 means FAIL. Tests
  self-check and set R1. Programs end with `BREAK`.
- **Boot from ROM:** programs assembled with `--org 0xFFFF0000`.
  `_start:` must be first label.
- **ROM page TLB mapping** (used by MMU tests):
  `TLB_INDEX=16 TLB_VPN=0x0FFFF000 TLB_PTE=0xFFFF00B9`.

## Benchmarks

Bare-metal benchmarks (Dhrystone 2.1, memtest, membench) under
`benchmark/`; built as PIE ELFs loaded from FAT32 via
`boot sd:0,0/DHRYSTON.ELF` etc. NetBSD-hosted microbenchmark suite
(`pbench`, syscalls + libc hot paths) under `benchmark/netbsd-bench/`.

- `make benchmark` — ISS run. `make benchmark-rtl` — Verilator
  (cycle-accurate, slower). `make benchmark-netbsd` — builds `pbench`
  which `make sdimage-rootfs` auto-overlays (along with `penmon`) into
  the rootfs `/usr/local/bin`.
- Override `BENCH_ITERS=`, `COPT="-Os"`, etc.
- Latest baseline numbers per snapshot (with HEAD SHA) live in
  `benchmark/netbsd-bench/BASELINE.md` — do not quote DMIPS/CPI/fmax
  from CLAUDE.md; numbers move and CLAUDE.md lags.

Harness details (cache enable, soft-float stubs, perfctr helpers) and
per-benchmark notes are documented inside each `benchmark/*/`
directory.

## Status and next steps

Current kernel/userland status is tracked in
`doc/system/netbsd/porting-status.md`. Hardware status (RTL,
testbenches, caches, FPGA bring-up) is documented per subsystem under
`doc/internals/` and `hw/CLAUDE.md`. Outstanding work and roadmap
items are in `doc/TODO.md`. Do not duplicate status here — those
documents are the source of truth.
