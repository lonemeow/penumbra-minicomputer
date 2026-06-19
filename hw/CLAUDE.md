# Penumbra Hardware — Claude Code Context

Navigation aid for work under `hw/`. The root `CLAUDE.md` has
project-wide conventions and pointers to architectural specs.

**Architectural specs** (read these before assuming RTL behavior):
- Microcode reference (micro-word format, all fields, full routine
  catalog, ROM organization, exception integration):
  `doc/internals/penumbra1/microcode.md`
- Datapath (three-bus architecture, signal flow): `doc/internals/penumbra1/datapath.md`
- MMU internals (TLB structure, fault flow): `doc/internals/mmu-internals.md`
- L2 cache design plan + phase status: `doc/internals/l2-cache.md`
- SDRAM controller v2 design and optimization:
  `doc/internals/sdram-{controller,optimization}.md`
- CPU-internal bus contracts (cpu_core ↔ MMU ↔ caches ↔ private sysregs):
  `doc/internals/cpu-bus.md`
- Bus protocol (signal-level handshake, sync/async forms):
  `doc/hardware/bus-protocol.md`
- ISA-visible behavior (exception priorities, vector numbers, SR bits):
  `doc/system/architecture.md`
- Sysreg device map: `doc/system/sysregs.md`
- RTL coding standards: `doc/internals/coding-standards.md`

## Directory layout

```
hw/
├── rtl/
│   ├── common/    # penumbra_pkg.sv + shared cells (tlb_pinned, ...) — both cores
│   ├── penumbra1/ # gen1 CPU core: datapath, regfile, ALU, sequencer, ROM, MMU, ...
│   ├── penumbra2/ # gen2 6-stage pipelined core + its MMU / L1 / arbiter / fill modules
│   ├── machine/   # machine integrations (machine_<generation>.sv)
│   ├── soc/       # Bus controller, autoconfig, caches (L1 VIPT, L1 PIPT, L2), boot ROM, cpuid/machid
│   ├── io/        # Real UART, real SPI, SDRAM v2 controller/adapter/PHY/CDC
│   ├── sim/       # machine_sim + machine_penumbra2_sim, sim devices, memory models
│   └── fpga/      # FPGA helpers (fpga_ram, lint stubs); board tops under <board>/
├── microcode/   # microcode.uasm (single source — assemble via uasm.py)
├── rom/         # Boot ROM (C + asm) and its standalone Makefile
├── sim/         # tb_cpu_prog (program runner), sim_console + per-core shims, per-module tbs
└── tools/       # uasm.py (microcode), oss-cad-suite wrappers
```

## RTL module map (one line per module)

Find a module by file path. For full module behavior, read the
source — the source is the authoritative description; this table is
purely a "where does this thing live?" index.

### Shared package and modules (`rtl/common/`)
Generation-neutral: everything here is used by both CPU cores. A
module instantiated by more than one generation lives here, never
cross-referenced from another generation's directory.
- `penumbra_pkg.sv` — shared constants (REG_*, ALU_*, COND_*, SR_*,
  VEC_*, FAULT_*, SYSDEV_*, SYSREG_*, CACHE_ADDR_*, UART_*, SPR_*,
  ACFG_*, base addresses). Imported by both CPU cores and every
  peripheral; the ISA contract for the whole system. Kept here, not
  under a generation directory, so it isn't generation-scoped.
- `cond_eval.sv` — 16 ARM-style condition codes.
- `byte_ext.sv`, `byte_rep.sv` — sub-word load extraction / store
  replication (defines byte-order convention).
- `divmul.sv` — MUL/DIV peer unit (sequential ~32-cycle iteration,
  own start/busy handshake; see `doc/internals/divmul.md`).
- `tlb_pinned.sv` — 8-entry FA pinned TLB, pinned-hit-wins. Parameterized
  for one or two translate ports (`DUAL_TRANSLATE`); both cores use it.

### Penumbra/2 core (`rtl/penumbra2/`)
The gen2 6-stage pipelined core (design in `doc/internals/penumbra2/`)
and its generation-bound memory-system modules. The bare core exposes
fetch/dmem front ports (launch / level-held request / busy-drop
completion), MMU query/verdict ports, and the sysreg sideband; the
machine integration (`rtl/machine/machine_penumbra2.sv`) binds them to
the MMU, the split VIPT L1s, the I/D arbiter + fill sequencer, and the
shared L2.
- `penumbra2_core.sv` — the bare core: front end (IF1/IF2) onto the
  spine, the vector-fetch FSM muxed onto the fetch port, the interrupt
  unit, and the memory/sysreg port groups.
- `penumbra2_spine.sv` — ID→EX→MEM→WB integration: stages, regfile,
  scoreboard, flag bypass, SPR file; fault-commit flush + save-state.
- `penumbra2_if1_stage.sv` / `penumbra2_if2_stage.sv` — split fetch (PC +
  BRAM address; deliver word + PC to ID).
- `penumbra2_id_stage.sv` / `penumbra2_decode.sv` — decode/issue; owns the
  scoreboard. `penumbra2_regmap.sv` maps arch regs → physical entries.
- `penumbra2_ex_stage.sv` — ALU + flag bypass + branch resolve + divmul +
  drain-commit; raises DIV0 / SYSCALL / BREAK.
- `penumbra2_mem_stage.sv` — alignment, single-STALL data access held
  through i_dmem_busy (hit: 2 cycles; miss/uncached: busy-drop
  completion), sub-word extract/replicate.
- `penumbra2_wb_stage.sv` — commit point: regfile/SR/SPR writes, takes the
  fault.
- `penumbra2_spr_file.sv` — SR / ESR / EPC; save-state, ERET restore, EI/DI.
- `penumbra2_scratch_file.sv` — SCR0..3 scratch SPRs (outside the regfile);
  one WB write port, one combinational ID read port.
- `penumbra2_vecfetch.sv` — exception vector-fetch FSM (handler address →
  registered PC redirect; waits out a busy fetch port on both ends).
- `penumbra2_irq.sv` — interrupt recognition + ei_shadow + drain-and-take.
- `penumbra2_alu.sv`, `penumbra2_regfile.sv`, `penumbra2_scoreboard.sv`,
  `penumbra2_flag_bypass.sv` — datapath leaf modules.
- `penumbra2_pkg.sv` — gen2-internal constants (scoreboard indices, op_class,
  alu_op, mem_op).
- `cache_bram_vipt.sv` — BRAM-backed VIPT L1 (launch/resolve front,
  4-way tree-PLRU, WT/WnA, atomic line fill, S_PT pass-through hold).
- `txn_arbiter.sv` — transaction-granular I/D arbiter (D-priority,
  single-outstanding, type-dependent completion).
- `fill_sequencer.sv` — atomic full-line fill walker between the
  arbiter and the shared L2.
- `penumbra2_mmu.sv` — gen2 MMU: MMUCR, per-port bypass, fault registers;
  registers the translation verdict at its output.
- `penumbra2_tlb_unit.sv` — combines the async-LUTRAM main TLB with the
  shared flop `tlb_pinned` (pinned-hit-wins).
- `penumbra2_tlb.sv` — async-LUTRAM main TLB (64-entry 2-way SA,
  combinational read; the verdict resolves in the access launch cycle).
- `penumbra2_tlb_perm.sv` — the 2-way match + permission verdict cone, one
  instance per translate port.

### Machine integrations (`rtl/machine/`)
One module per generation: the board-independent computer
(`doc/internals/build-system.md`). Devices attach outside, on the
exposed external bus.
- `machine_penumbra2.sv` — gen2 core + penumbra2_mmu + 2× cache_bram_vipt +
  txn_arbiter + fill_sequencer + l2_cache + the sysreg device complex
  (MMU, both L1s, L2, cpuid, scratch); exposes the external bus, IRQs,
  commit/retire, and the program-end pulse (a retiring BREAK).
  `machine_penumbra1` (extraction from `machine_sim` /
  `ulx3s_penumbra1_top`) is still to come.

### Penumbra/1 core (`rtl/penumbra1/`)
- `cpu_core.sv` — full CPU integration: datapath + sequencer + ROM
  + MMU + split I/D L1 cache + memory bus mux + fetch + IRQ + traps
  + WRSYS/RDSYS + RDSPR/WRSPR + sub-word loads/stores + perfctr.
  Parameterizable `RESET_PC` (default `0xFFFF_0000`).
- `datapath.sv` — structural wiring of all datapath modules.
- `sequencer.sv` — micro-PC, branch_cond decode, EI/DI tracking.
- `ucode_rom.sv` — 256×51-bit ROM (`$readmemh` from microcode.hex).
- `alu.sv` — unified compute (11 single-cycle ops, multi-cycle stubs).
- `regfile.sv` — 2R/1W, R0=zero, R14 banked USP/SSP, R15→PC, debug port.
- `imm_ext.sv`, `field_ext.sv` — immediate / IR-field extraction.
- `bmux.sv`, `wmux.sv`, `amux.sv`, `pc_mux.sv` — datapath muxes.
- `status_reg.sv`, `pc_reg.sv`, `mar.sv`, `mdr.sv` — core state regs.
- `cpu_perfctr.sv` — cycles + insns_retired counters, exposed via
  `SYSDEV_CPU` regs 5+.
- `cpu_bus_arbiter.sv` — serializes split I/D L1 traffic onto the
  single external bus. 2-state FSM with back-to-back BUSY→BUSY
  re-latch; combinational `o_*_req_accepted` pulse for burst
  address advance.
- `mmu.sv` — gen1 MMU: bypass/translate mux, `force_bypass` for vector
  fetch, alignment check, sysreg routing, fault latching.
- `tlb_unit.sv` — gen1 unified main + pinned lookup behind one interface.
- `tlb.sv` — gen1 64-entry 2-way SA main TLB (distributed-RAM, parallel
  combinational lookup, one-hot permission check, indexed sysreg R/W).

### SoC and caches (`rtl/soc/`)
- `cache_vipt.sv` — L1 cache (used for both I and D in `cpu_core`).
  Index/word from vaddr, tag compare from paddr; aliasing-free by
  the cache-size ≤ page-size precondition (asserted at sim time).
  Write-through / write-no-allocate. Burst line fill on read miss
  uses `req_idx`/`resp_idx` pipelined against the arbiter's
  `i_req_accepted` handshake.
- `cache.sv` — original PIPT cache, retained for the L2-style use
  case (`doc/internals/l2-cache.md`). Not currently instantiated.
- `l2_cache.sv` — L2 phase 1: 64 KiB, 4-way, 16 B lines, tree-PLRU,
  2-cycle hit pipeline. Write-invalidate-on-hit (write-back is a
  planned phase). Disabled at reset; software enables via
  `WRSYS SYSDEV_L2_CACHE CTRL=1`.
- `busctl.sv` — SYSDEV_BUS sysreg device: RST (sticky) + CFG_EN for
  autoconfig.
- `autoconfig_dev.sv` — config-space regs, cfg daisy chain with
  CFG_EN toggle, dynamic base-address decode.
- `bus_devsel.sv` — combinational address comparator (BASE/SIZE
  parameterized).
- `boot_rom.sv` — ROM (64 KB default), `$readmemh` from `program.hex`
  (boot ROM owns that root-level name; sims override per program via
  the `+rom_hex=<path>` plusarg, which the Makefile points at
  `build/hex/<prog>.hex`).
- `cpuid.sv` — read-only CPU identity (sysreg device 1, regs 0–4).
- `machid.sv` — read-only board identity (sysreg device 8).

### I/O peripherals (`rtl/io/`)
- `uart.sv` — NS16550A-compatible. Two-stage baud generator
  (fractional accumulator synthesizes 1.8432 MHz reference from
  `CLK_FREQ`, then divisor stage produces 16× baud). 16-byte FIFOs
  gated by FCR[0]; bypass to 16450 single-byte mode when FCR[0]=0.
  Trigger 1/4/8/14, character-timeout interrupt, BREAK detection.
- `spi.sv` — SPI master v2 with hardware TX/RX FIFOs, autonomous
  transfer engine (stall-on-empty/full), IRQ. See
  `doc/system/devices/spi.md` for register map and driver flow.
- `spi_fifo.sv` — parameterized sync FIFO (used by UART + SPI).
- `sdram/sdram_pkg.sv` — command encoding + chip presets.
- `sdram/sdram_ctrl.sv` — FSM core: init / refresh / ACT/RW/RECOVER.
  `PHY_OUT_LATENCY` / `PHY_IN_LATENCY` parameters absorb
  registering-PHY pipeline.
- `sdram/sdram_bus_adapter.sv` — sync bus ↔ controller req/rsp;
  pipelined cache fills + speculative `addr+4` prefetch on accepted
  reads (depth-2 CDC carries real + spec; 1-bit tag FIFO routes
  responses).
- `sdram/sdram_cdc.sv` — depth-2 async FIFO bridge across the
  CPU↔SDRAM clock domains. Up to 2 outstanding, order-preserving;
  Gray pointers via 2-FF synchronizers; data-before-valid CDC.
- `sdram/sdram_phy_sim.sv` — pass-through PHY for Verilator.
- `sdram/sdram_phy_ecp5.sv` — IOB flops + ODDRX1F clock forward.
  `i_clk` is CLKOS @ 0°, `i_clk_sdram` is CLKOS2 @ ~270° (see
  `doc/internals/sdram-controller.md` and `sdram-optimization.md`).

### Simulation glue (`rtl/sim/`)
- `machine_penumbra2_sim.sv` — `machine_penumbra2` + `unified_bus_mem`:
  the gen2 program-runner wrapper (`make test CORE=penumbra2`).
- `unified_bus_mem.sv` — bus-shaped unified ROM/RAM device (region-
  compressed map, registered read with 1-cycle busy, `+rom_hex=`).
- `unified_mem.sv` — dual-port flat stand-in for core-level bring-up
  (registered read, no busy; the machine made it spare).
- `machine_sim.sv` — `cpu_core` + `boot_rom` + `simple_mem` +
  `sim_uart` + `machid` + `busctl` + autoconfig SPI. Shared-bus
  via `bus_devsel`, UART IRQ wired.
- `sim_uart.sv` — NS16450-compatible UART for sim (no FIFO).
- `sim_spi.sv` — SPI master + SD card emulator (via `+sdcard=`).
- `sdram_sim.sv` — wraps adapter + ctrl + `phy_sim` + chip model
  into a `simple_mem`-shaped device.
- `sdram_test.sv` — DUT wrapper exposing controller req/rsp for
  `tb_sdram_test`.
- `sdram_model.sv` — behavioral SDR DRAM chip (JEDEC command set,
  sparse storage, protocol checking).
- `simple_mem.sv` — parameterizable sync SRAM model (default 16 MB),
  configurable READ_LATENCY/WRITE_LATENCY.

### FPGA top-levels (`rtl/fpga/`)
Board tops live under `rtl/fpga/<board>/`, one file per registered
(board, core[, variant]) combination — built via
`make fpga BOARD=<board> CORE=<generation>` (registry: `FPGA_TOPS`
in the root Makefile; naming spec in `doc/internals/build-system.md`).
A microarch sub-variant (e.g. `CORE=penumbra2_5`) is registered too but
has no file of its own: it reuses its base board top synthesized with
one core parameter set, so `TOP` names the artifact and `TOP_MODULE`
the shared module.
- `ulx3s/ulx3s_penumbra1_top.sv` — gen1 system on the ULX3S.
  12.5 MHz PLL (25 MHz crystal), 32 MB SDRAM (W9825G6KH or
  compatible), real UART (TX+RX), real SPI with SD card (autoconfig),
  boot ROM, `btn[1]` reset.
- `ulx3s/ulx3s_penumbra2_probe_top.sv` — gen2 machine timing probe
  (`VARIANT=probe`): `machine_penumbra2` + a BRAM bus memory at
  25 MHz, terminal outputs folded onto the LEDs so synthesis keeps the
  design, IRQ inputs on `btn[2]`/`btn[3]`. The first build with the
  IF2 tag-compare/way-mux path and the L1↔L2 layer in front of nextpnr
  (`make timing BOARD=ulx3s CORE=penumbra2 VARIANT=probe`), not a
  usable machine.
- `fpga_ram.sv` — BRAM-friendly memory (4 byte-wide banks with
  `ram_style` attribute).

## Boot ROM (`rom/`)

Penumbra/1 boot monitor in C. No `.bss`/`.data` — ROM has no writable
data section; all state lives in boot data or on the stack.

| File | Purpose |
|------|---------|
| `boot_rom.c` | `main()`, trap setup, RAM detection, bus autoconfig, monitor loop |
| `console.{c,h}` | Line-editing input (`console_gets`), formatted output (`console_printf`, `console_puts`) |
| `sdcard.{c,h}` | SD-SPI protocol (init/deinit/read_sector/detect), MBR parsing, probing |
| `fat32.{c,h}` | Minimal read-only FAT32 (mount, root-dir search, cluster-chain read). Device-independent via `blk_read_fn` callback |
| `elf.h` | Minimal ELF32 header defs (Ehdr/Phdr/constants) for PIE loading |
| `util.{c,h}` | Unaligned LE reads, size formatting |
| `bootdata.h` | Boot data tagged list builder/lookup (inline, header-only) |
| `spi.h`, `penumbra.h` | Inline-only SPR/sysreg/SPI register accessors and constants |
| `libc.{c,h}` | Minimal C library (strlen, strcmp, strtoul, snprintf, soft mul/div) |
| `uart.{c,h}` | UART polling driver |
| `crt0.s` | Startup: preload UART, run RAM diag, set SP, call `main` |
| `ram_check.s` | Pre-stack SDRAM controller diagnostic + early-print helpers |
| `trap_entry.s` | Exception trampolines |
| `rom.ld` | Linker script |
| `Makefile` | Own build (auto source discovery + header deps). Pipeline: `clang -c` → `llvm-mc` → `ld.lld` → `llvm-objcopy` → `bin2hex.py` |

**Monitor commands:** `boot sd:<dev>,<cs>[/file]` (mount FAT32, load
named file or `PENBOOT.ELF` by default as PIE, allocate RAM, copy
PT_LOAD, jump), `x <addr> [len]` (hex dump),
`load sd:<dev>,<cs>[:<part>] <addr> <lba> <count>`,
`part sd:<dev>,<cs>` (MBR table), `go <addr>` / `g <addr>` (jump
with R1=boot data), `break` / `b` (halt). SD naming uses per-class
controller index (`sd:0,0` = first SD controller, CS0).

## Interactive console (`sim/sim_console.cpp` + per-core shims)

`sim_console.cpp` is the generation-independent frontend: raw terminal
mode, the UART hold-until-ack RX feed, TX→stdout + the `+halt_on=`
matcher, the SD↔`SdCardSim` bridge, `+stdin_file=` replay + a throttled
stdin poll, trace-file management, and the main loop. It includes no
Verilated header — it drives a `SimCore` (`sim_console.h`). Each
generation supplies a thin shim that owns the DUT: `tb_penumbra1_interactive.cpp`
(`Vmachine_sim`, 4:1 SDRAM clock, full `o_trace_*` + `o_dbg_reg`
instruction trace) and `tb_penumbra2_interactive.cpp`
(`Vmachine_penumbra2_sim`, single clock, BREAK via `o_prog_end`; no
trace ports yet, so `+trace=` is ignored). Both are built by
`make simulate-rtl [CORE=…]`. Plusargs: `+sdcard=disk.img` (SDCARD),
`+trace=file.log` (TRACE), `+trace_window=N`, `+halt_on=str`,
`+stdin_file=path`.

## Implementation gotchas

### Device `o_busy` contract (registered-read devices)
The CPU's STALL sequencer exits and latches `mem_rdata` on the cycle
when `o_busy` drops. **Any device with registered read output
(1+ cycle latency) must assert `o_busy` for at least one cycle on
reads** — otherwise data is not valid when busy clears, and the CPU
latches stale/zero.

Use the `access_pending` pattern from `sim_uart.sv`:
`o_busy = i_re && !access_pending`. A device that reports
`o_busy = 0` immediately while having registered read data caused a
real bug in `sim_spi.sv`.

### Memory bus
- STALL-based load/store: same microcode regardless of memory
  latency. STALL also checks `i_mem_fault` alongside `i_mem_busy`;
  on fault, sequencer aborts to `S_FETCH`.
- Split I/D L1 caches share the external bus via `cpu_bus_arbiter`
  (D-priority on simultaneous pending).
- See `doc/internals/cpu-bus.md` for the full cpu_core ↔ MMU ↔ cache
  contract (handshakes, priority, ordering).

### Microcode
- Field semantics, ROM zone layout, sequencer behavior, exception
  integration, and the full implemented-instruction catalog all live
  in `doc/internals/penumbra1/microcode.md`. Do not duplicate any of that here.
- ALU and SYS-format µ-words share R-format encoding split by op[4]
  (ALU: 0x00–0x1E; SYS: 0x40–0x5E). Format M uses ×4 slot spacing
  (0x80–0xBF). Microcode assembler validates slot boundaries.

### Exception flow
Eight sources share `except_entry → int_entry → vector dispatch`:
external IRQ (dispatch-time check, gated by SR.I and `ei_shadow`),
MMU data fault (STALL-time), MMU fetch fault (S_FETCH-time),
alignment (any access, before TLB lookup), bus fault (no device at
address — wired from `machine_sim` into `cpu_core.i_bus_fault`),
BREAK, SYSCALL, privilege violation, illegal instruction.

Priority: `fault > illegal > priv > BREAK > SYSCALL > IRQ`. Vector
numbers and the dispatch sequence (EPC/ESR save, mode switch, vector
fetch via MMU bypass) are documented in `doc/internals/penumbra1/microcode.md`
(§ Exception Integration).

### Register address routing
Micro-word `reg_a_sel`/`reg_b_sel`/`reg_w_sel` use a 4-bit encoding:
`4'b0000=IR_RD`, `4'b0001=IR_RS`, `4'b0010-1111=R2-R15`. F-bit
write-enable gating applies only when `reg_w_sel = IR_RD` (not for
literal addresses).

## FPGA toolchain (`tools/oss-cad-suite/`)

Tools for synthesis, PnR, bitstream packing, and flashing are
containerized in `tools/oss-cad-suite/`, Docker image based on
Ubuntu 22.04 + OSS CAD Suite release.

Wrapper symlinks in `tools/oss-cad-suite/bin/` (all resolve to
`docker-wrapper.sh` — multi-call pattern): `yosys`, `nextpnr-ecp5`,
`ecppack`, `fujprog`. Container runs as host UID to avoid root-owned
output. `fujprog` requires USB passthrough (`--privileged`,
`/dev/bus/usb`).

Build flow and `make fpga`/`make flash`/`make timing` commands are
documented in the root `CLAUDE.md`.
