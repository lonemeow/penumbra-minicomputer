# Penumbra Minicomputer — Claude Code Context

Penumbra is a 32-bit RISC-like minicomputer designed from scratch and
implemented on a Radiona ULX3S (Lattice ECP5) FPGA — CPU, MMU, I/O and
system bus. It boots NetBSD to userland on the FPGA today; the
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
| Instruction costs per generation, typical memory costs | `doc/system/instruction-timing.md` |
| ABI, ELF relocations, calling convention | `doc/system/abi.md` |
| LLVM backend design notes, upstream gap write-ups | `doc/llvm/` |
| MMU, sysregs, bus address map, boot protocol | `doc/system/{mmu,sysregs,bus,boot-protocol}.md` |
| Device register maps, one per device class | `doc/system/devices/` |
| NetBSD port — current status, kernel design | `doc/system/netbsd/{porting-status,design-notes}.md` |
| Bus signal-level protocol (hardware) | `doc/hardware/bus-protocol.md` |
| Penumbra/1 datapath, microcode, L1 cache | `doc/internals/penumbra1/` |
| MMU internals, L2 cache, CPU-internal bus, system-bus implementation | `doc/internals/{mmu-internals,l2-cache,cpu-bus,system-bus}.md` |
| Penumbra/2 (gen2) pipelined-core design | `doc/internals/penumbra2/` |
| SDRAM controller / optimization | `doc/internals/sdram-{controller,optimization}.md` |
| A board's own realization (tops, clocking, memory, constraints) | `doc/internals/<board>/` |
| RTL coding standards | `doc/internals/coding-standards.md` |
| Current TODO / roadmap | `doc/TODO/` (index: `doc/TODO/README.md`) |

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
- Every memory declares its storage (`ram_style`/`rom_style`) and is
  written in a form both yosys and Vivado map the same way; the forms
  are in the coding standards ("Toolchain Compatibility").
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

### TODO entries
Open work lives in `doc/TODO/`, one file per area; the table in
`doc/TODO/README.md` says which area each file covers.
- **Where:** the file of the subsystem that owns the fix. Core,
  bus, cache, SDRAM and fmax work share `hw-core.md` because those
  items gate each other. A workload profile whose levers span
  subsystems goes in `workloads.md`, and the subsystem entries link
  to it. Work bound to one board goes in `hw-<board>.md`, unless it
  also creates a programming interface: then it lives with the
  interface in `hw-devices.md` and names the boards it serves. Add a
  new file (and a README row) only for a genuinely new area.
- **Shape:** one `## <Area>: <problem>` entry per item, stating the
  problem, the evidence, and what is still open. Link other entries
  by file and heading, never "above"/"below".
- **Findings** that motivate open work go in the entry. A finding
  that describes how the system behaves, with nothing left to do,
  belongs in the subsystem's doc under `doc/internals/` (hardware),
  `doc/llvm/` (compiler) or `doc/system/`, not in TODO.
- **Completed:** delete the entry rather than marking it RESOLVED.
  Before deleting, move what stays useful: the reasoning to a comment
  at the fix, what changed and what it measured to the commit
  message, protection to a regression test, and a lesson that
  generalizes to the subsystem doc. A partly done entry keeps only
  the open remainder plus one line naming the commit that landed the
  rest. Fix every inbound reference in the same change
  (`grep -rn 'doc/TODO/<file>'`).
- **Ruled out:** an approach tried or analysed and declined stays as
  a short entry (what, measured result, why declined, commit or doc
  link) under `## Ruled out` at the end of the owning file, so it is
  not re-proposed.

## Build System

### Simulation and tests
- `make simulate` — boot ROM + ISS, interactive (fast, no Docker).
  Options: `LLVM_PREFIX=`, `SDCARD=disk.img`,
  `USBDEV=<type>:<config>` (attach a simulated USB device, e.g.
  `USBDEV=disk:disk.img`; hot-plug at runtime with `Ctrl-A U` under
  `RAW=1`), `ETH=<spec>` (attach an Ethernet adapter, the ISS's
  `+eth=`, e.g. `ETH=loop`), `TRACE=trace.log`,
  `RAW=1` (passes Ctrl-C etc. to guest; Ctrl-A is escape prefix —
  `Ctrl-A X` exit, `Ctrl-A C` CPU state, `Ctrl-A H` help).
  ROM monitor accepts `break` (or `b`) to halt cleanly.
- `make simulate-rtl` — boot ROM + Verilator RTL sim, interactive via
  Docker (`-it`). Cycle-accurate but slow. `INTERACTIVE=0` for piped
  input. Same `SDCARD`/`TRACE` options.
- `make test-iss` — the `hw/sim/programs/isa/` conformance suite on
  the ISS (no Docker).
- `make test-libc` — the hand-written string routines libc and the
  kernel share (`netbsd/common/lib/libc/arch/penumbra/string/`), on the
  ISS against reference loops at every alignment (`test/libc/`).
- `make test-kernel` — the kernel's MD routines that touch only
  registers and sysregs (the TLB invalidation in
  `netbsd/sys/arch/penumbra/penumbra/tlb_subr.S`), assembled as the
  kernel build does and run on the ISS against their contracts
  (`test/kernel/`).
- `make test-iss-options` — the ISS's own options (`+core=`, …)
  against programs with known answers (`sw/sim/tests/`); expected
  values are written from the hardware's parameters.
- `make test [CORE=penumbra1|penumbra2]` — `isa/` + the core's
  microarch suite (`hw/sim/programs/<core>/`) on RTL sim via Docker.
  `CORE` also takes a microarch sub-variant (`penumbra<n>_<sub>`, e.g.
  `penumbra2_5`): the base generation built with one core parameter set,
  inheriting its suite (mechanism in `doc/internals/build-system.md`).
  `SIM_MEM=ddr3` (gen2 only, here and for `simulate-rtl`) runs the
  machine on the Nexys Video's DDR3 memory stack, 512 MiB, in place of
  the ULX3S's SDRAM.
  `make test-prog CORE=<core> PROG=<name>` runs one program. Programs
  carry lit-style `; RUNNER:` / `; REQUIRES:` header tags (scanned by
  `hw/tools/run-prog-tests.py`); suite layout and tag semantics in
  `doc/internals/build-system.md`.
- `make test-modules` — module-level Verilator testbenches (alu,
  regfile, cache_test, sdram_adapter_test, …). List is `MODULE_TESTS`
  in the Makefile; integration testbenches (`tb_cpu_prog`,
  `tb_interactive`) are intentionally excluded. Add new entries when
  introducing modules with a `tb_<mod>.cpp` or `<mod>_test.sv` wrapper.
- `make test-gate` — gate-level event simulation of the synthesized
  `sdram_cdc` with a routing delay on every cell input (Icarus, via the
  FPGA container). Guards clock-domain crossings against combinational
  glitch hazards, which a zero-delay simulator cannot represent at all.
  Harness in `hw/sim/gate/`; `GATE_SEEDS`/`GATE_CYCLES` size the run
  and `GATE_WIDTHS` lists the response payload widths to cover.
- `make test-all` — `make test` then `make test-modules`, `test-rom`
  and `test-gate`.
- `make test-compiler` — `llvm-test-suite` C tests on ISS in `+hosted`
  mode. Requires `compiler-rt` built per optimization level
  (`sw/tools/setup-compiler-rt.sh` builds `-O0` and `-O2` archives;
  the suite links the one matching `OPT`). Override `OPT=` or
  `COMPILER_TESTS=path/to/test.c`. Report in
  `build/test-compiler-report.txt`. Excludes and deferred backend gaps
  in `test/compiler/excludes.txt` and `doc/TODO/compiler*.md`.
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
nextpnr-ecp5 and ecppack usable as normal commands:
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
- `SDRAM_STACK=word|line` — which shape of SDRAM stack a top builds.
  `line` swaps the bus adapter and stack for the variants in
  `hw/rtl/io/sdram_line/`, so one controller command returns a whole
  burst and the adapter holds it; the controller, CDC and PHYs are
  shared. Both define the same modules with the same ports, so no board
  top mentions the choice. The default is per top —
  `SDRAM_STACK_<top>` in the Makefile, beside that top's placement
  seed, since which stack closes timing is a placement question.
- `make fpga-lint` — Verilator lint check (full gen1 system).
- `make timing BOARD=ulx3s CORE=penumbra1 [TOP_N=10]` — pretty-print
  fmax + top critical paths from `build/<top>_timing.json`.
- gen2 timing checks: `make fpga`/`timing` with
  `BOARD=ulx3s CORE=penumbra2 VARIANT=probe` builds the bare-core
  probe top — run after gen2 RTL changes to catch fmax movement early.

Board top-levels under `hw/rtl/fpga/ulx3s/`: `ulx3s_penumbra2_top.sv`
(the default core, built as gen2.5) and `ulx3s_penumbra1_top.sv`.
Serial 115200 8N1 on `/dev/ulx3s-console` (udev rules in
`hw/tools/udev/`). SD slot autoconfigured as
`CLASS_SD`; `boot sd:0,0` loads `PENBOOT.ELF` from FAT32. Toolchain
mechanics (wrapper multi-call, sv2v + `$readmemh` workaround, USB
passthrough) in `hw/CLAUDE.md`.

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
- Kernel build: `build.sh kernel=GENERIC.DEBUG` from `netbsd/` (the
  dev default — consistency checks on; `GENERIC` for demo/perf images,
  `MINIMAL` for the smallest bootable config); output lands at
  `build/netbsd-obj/sys/arch/penumbra/compile/<config>/netbsd`, and
  the image targets pick the config via `KERNCONF=` (default
  `GENERIC.DEBUG`). Full
  sequence in `DEVELOP.md` § 7.
- SD images: `make image [FLAVOR=multiuser|kiosk]`, built by NetBSD's
  own `distrib/utils/embedded/mkimage` from a board conf per flavor
  under `netbsd/distrib/utils/embedded/conf/`. Adding a flavor means
  adding a conf. Images carry `boot.cfg` with `root=ld0f` so
  `boot sd:0,0` needs no prompts, plus the bare-metal benchmark ELFs
  on the FAT32 partition (`boot sd:0,0/DHRYSTON.ELF`). Requires NetBSD
  cross-tools built once via `build.sh tools`. Custom userland
  utilities (benchmark suite, `penmon`, `exhibit-launcher`) reach
  `/usr/local/bin` via each tool's `overlay` make target staged into
  `build/netbsd-overlay`; see DEVELOP.md for the convention and how to
  add a utility.
- The set lists under `build/netbsd-dest/etc/mtree/` are the image's
  file inventory and go stale if anything installs into DESTDIR
  outside a full `distribution` build; `makefs` then refuses the files
  that changed. DEVELOP.md covers repairing it.

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
  `sw/sim/penumbra-iss program.hex [+ram=size] [+sdcard=img]
  [+usbdev=disk:img] [+trace=log] [+raw] [+trap-pc0] [+core=gen]`.
  `+ram=` sizes RAM (32M, the ULX3S's, by default; `+ram=512M` for the
  Nexys Video; `RAM=` on `make simulate` and `make benchmark`).
  `+trap-pc0` aborts on PC=0 fetch (catches null-pointer
  jumps; off by default because some OS tests probe address 0).
  `+core=` picks the hardware generation the ISS presents, spelled as
  `make CORE=` spells it (`penumbra1` by default, `penumbra2`,
  `penumbra2_5`): its CPU name and the geometry its caches report.
  The table is `sw/sim/penumbra_cores.h`; a generation's cache or
  TLB geometry or CPU name changing in RTL has to change there too.
  `+eth=backend[,mdio][,rx=N][,tx=N][,speed=MBIT]` attaches an
  Ethernet adapter (`CLASS_ETHERNET`,
  `doc/system/devices/ethernet.md`) with N slots in each ring; the
  machine has none without it. The `loop` backend hands every frame
  the device sends to its own receive side, which is what most
  `eth_*.s` cases of `make test-iss-options` drive.
  `tap[:name]` exchanges frames with a Linux TAP device, so the host's
  network stack is the guest's peer. `sw/tools/iss-net.sh` runs a
  command (`make simulate ... ETH=tap`) in a namespace where making
  that device needs no root, and with `--join` runs a second one there
  (`ping`, `tcpdump`). The adapter manages its own link unless `mdio`
  gives it a PHY for software to manage, a generic gigabit one; then
  `speed=` is the fastest the device itself carries, which may be
  less than the PHY settles on.
  Measurement options: `+opstats` (instruction-class histogram),
  `+branchstats`, `+pcprof`, `+cachemodel`, `+tlbmodel`, `+sbmodel`,
  `+bpmodel` and `+pairmodel` — see below.
- **Cache model** (`+cachemodel[=list]`, `sw/sim/cache_model.h`) —
  runs every fetch, load and store through shadow cache hierarchies:
  the `+core=` generation's by default, or a comma-separated list of
  generation names and explicit `L1I:L1D:L2` levels as `SIZE/WAYS`
  (`8k/2:8k/2:64k/4`; 16-byte lines).  Each level follows the RTL's
  replacement, write and enable behaviour, and each window reports the
  counts the cache perfctrs keep plus L1 fills by kind, served from L2
  or from memory, and the L1-I misses a next-line prediction buffer
  (proposed for gen3) would have served.  A window opens at reset or at
  `+model_start=ADDR` and closes at `+model_stop=ADDR` (both first
  user-mode execution, as `+pcprof_start`), on `SIGUSR1` (which opens
  the next), or at exit; `+model_report=file` writes the reports there
  instead of stderr.  **Counts are architectural** — no wrong-path
  fetches, no timing: multiply by costs measured on RTL or hardware for
  cycles.
- **TLB model** (`+tlbmodel[=list]`, `sw/sim/tlb_model.h`) — replays
  every successful main-TLB translation (pinned slots excluded) through
  shadow TLBs: the `+core=` generation's by default, or a list of
  generation names and `SETSxWAYS` layouts, each optionally `/lru`
  (default random, as a counter-picked refill way is) and `/hash`
  (VPN XOR-folded into the index): `+tlbmodel=penumbra2_5,64x2,256x2`.
  Each window, in the same report as the cache model's, gives each
  shadow's misses beside the refills the running kernel actually took;
  the built layout's shadow calibrates the rest.  Shadows drop only
  what software invalidates in the real TLB, so ones larger than it
  read slightly optimistic.
- **Store-buffer model** (`+sbmodel[=list]`, `sw/sim/sb_model.h`) —
  runs every store through shadow store buffers of gen3's planned shape,
  each on its own estimated clock: `+sbmodel_cpi=` cycles an
  instruction (1 by default) plus fill and uncached-access costs, with
  fills taken from the first `+cachemodel` hierarchy (so it needs
  `+cachemodel`).  Items are `ENTRIES{l|w}/POLICY/DRAIN`: line or word
  entries, fills waiting for `all` entries, for those up to their
  `line`, or for none as they `merge` buffered bytes, and drain costs
  as a preset (`bus`, `combine`) or four cycle counts by burst length
  (`8l/line/5:7:9:11`); a trailing `/post` posts write-through past L2,
  so a fill L2 serves waits only for L2 writes.  Each window reports
  merges, drains by burst length, and the cycles the core waits on a
  full buffer and on drains before I fills, D fills and uncached
  accesses.  **Waits are an upper bound:** nothing else a pipeline
  stalls on is modelled, so stores arrive closer together than on
  hardware.
- **Branch-predictor model** (`+bpmodel[=list]`, `sw/sim/bp_model.h`) —
  runs every control transfer through shadow predictors and counts
  where each would have redirected fetch: at fetch from a target
  buffer, at the IF2 pre-decode, or in EX.  Items join components with
  `+`: `btbN` (direct transfers, as gen2.5's) or `ibtbN` (indirect too),
  `bimN` or `gsN` (bimodal or gshare two-bit counters), `rasN` (return
  stack, a plain ring) or `rasNe` (one that predicts nothing when
  empty), `dec` (pre-decode redirects of direct transfers, BTFN without
  a direction table), or `none` (`+bpmodel=btb32,ibtb256+gs1k+ras8+dec`).
  Each window reports transfers by kind, redirects by stage and the
  cycles they cost; the counts are exact, the per-stage costs estimates.
- **Pairing model** (`+pairmodel[=list]`, `sw/sim/pair_model.h`) —
  runs every instruction through shadow two-slot issue stages and
  counts the issue cycles each would take: which instructions issue as
  a pair, which fuse into one op, and what kept the rest apart.  Items
  start with `single` or `dual` (slot X issues nothing, or the simple
  ALU classes; Y everything) and add modifiers with `+`: `x:` / `y:`
  class lists (`alu`, `sh`, `mov`, `adc`, `ld`, `st`, `br`, `md`, or
  `simple`, `all`, `none`), `fw:` same-cycle forwards (`cond`,
  `carry`, `mov`, `imm`, `data`, `ea`, `alu`), `fuse:` fusions
  (`lilui`, `cmpbr`, `opbr`, `movop`), `reach:any|line|pair` (which
  adjacent instructions the fuser sees: any two, two in one line, or
  an aligned pair), `mem:1|ld2|2`, `flex` (either op in either slot),
  `yflags` (only the younger op commits flags), and a fetch stage:
  `fetch:N` or `fetch:Nl` (up to N instructions a cycle into a queue,
  `l` ending a fetch at the end of its line), with `bubble:N` (cycles
  fetch idles after a redirect, 1 unless given) and `queue:N` (8):
  `+pairmodel=dual,dual+fw:cond:mov:imm+fuse:lilui+fetch:2l`.  Each
  window reports cycles by instructions issued, pairs by the forward
  they rely on and by the younger op's class, and singles by what kept
  the next op out: one cause alone, or several.  A stage with a fetch
  stage also reports its idle cycles, fetches, bubbles and cycles the
  queue was full.  **Counts are issue slots only:** no stall, miss or
  misprediction is modelled, and without `fetch:` every instruction is
  at hand when issue wants it.
- **Execution profiles** (`+pcprof=file` plus
  `sw/tools/pcprof-report.py`) — buckets executed instructions by PC,
  exceptions by vector and TLB misses by faulting page.
  `+pcprof_start=ADDR` holds off until that PC is first executed in
  user mode, which is how a profile skips boot and level loading:
  give it the address of the workload's own frame or iteration entry.
  Pair it with `+halt_on=STR` to stop at the workload's report line.
  **Counts are architectural** — the ISS models no cache or memory
  latency, so rows weigh instructions executed, never cycles.

  The report resolves PCs against the images that were running; a
  shared library or code copied to a runtime address carries its load
  base after an `@`:

  ```sh
  sw/tools/pcprof-report.py prof.txt build/doom/doom \
      $KERN $KERN@$((0xffffb080 - <&_real_miss_handler>)) \
      build/netbsd-dest/lib/libc.so.12@<base> --frames 149
  ```

  The second kernel mapping attributes the TLB refill handler, which
  runs from the pinned vector page at `0xFFFFB080` rather than where it
  was linked. Unattributed PCs are listed by page so the missing image
  is identifiable; a library's base has to be supplied (nothing in the
  profile records it).
- **Unattended NetBSD runs** (`sw/tools/iss-workloads.py`) — boots an
  SD image from the ROM monitor, reaches a root shell (multiuser login
  or the single-user prompt) and runs each workload command in turn,
  typing each character only after the last one echoes: the modelled
  UART holds one byte, so a script piped into stdin loses characters.
  `--windows` gives each workload its own model window (`SIGUSR1`
  around the command) and says which window is which; ISS options pass
  through `--iss-arg` (`--iss-arg +core=penumbra2_5 --iss-arg
  +cachemodel`).  The run writes to the image, so drive a copy.
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

Bare-metal benchmarks (Dhrystone 2.1, CoreMark 1.0, memtest, membench)
under `benchmark/`; built as ELFs loaded from FAT32 via
`boot sd:0,0/DHRYSTON.ELF` etc. — PIE, except Dhrystone and CoreMark,
which link statically as the builds published scores come from do.
NetBSD-hosted microbenchmark suite (`pbench`, syscalls + libc hot
paths) under `benchmark/netbsd-bench/`.
Graphics demos live in `sw/demos/` — they share `perfctr.c` with pbench
but are exhibit material rather than measurements.

- `make benchmark` — ISS run. `make benchmark-rtl` — Verilator
  (cycle-accurate, slower). `make benchmark-netbsd` — builds `pbench`;
  `make demos` builds the graphics demos. `make image` auto-overlays
  both (along with `penmon` and `exhibit-launcher`) into the rootfs
  `/usr/local/bin`.
- Override `BENCH_ITERS=`, `COPT="-Os"`, etc.
- The bare-metal benchmarks link the `-O2` `compiler-rt` builtins
  (`sw/tools/setup-compiler-rt.sh`), which give the harness 64-bit
  division: harness arithmetic is `uint64_t`, never 32-bit with an
  argument about what fits.
- **CoreMark** (`COREMARK.ELF`) sizes its own runs to the ten seconds
  its run rules ask for, so it is in the ISS loop only. The sources in
  `benchmark/coremark/` are EEMBC's and may not change (the build
  checks `coremark.md5`); the port is `benchmark/coremark/penumbra/`.
  Its per-MHz reporting line states the memory, which no register
  describes: `machine_memory[]` in `benchmark/common/bench.c` restates
  each board's memory type and clock, and has to follow the board.
- The repo keeps no reference numbers: measure on the FPGA, running the
  build you compare against in the same session.  A DMIPS/CPI/fmax
  figure quoted in any doc, CLAUDE.md included, is a dated sample.

Harness details (cache enable, soft-float stubs, perfctr helpers) and
per-benchmark notes are documented inside each `benchmark/*/`
directory.

## Status and next steps

Current kernel/userland status is tracked in
`doc/system/netbsd/porting-status.md`. Hardware status (RTL,
testbenches, caches, FPGA bring-up) is documented per subsystem under
`doc/internals/` and `hw/CLAUDE.md`. Outstanding work and roadmap
items are in `doc/TODO/` (see [TODO entries](#todo-entries)). Do not
duplicate status here — those documents are the source of truth.
