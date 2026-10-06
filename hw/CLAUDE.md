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
- System bus implementation (decode placement, response fabric, device
  handshake cells): `doc/internals/system-bus.md`
- ISA-visible behavior (exception priorities, vector numbers, SR bits):
  `doc/system/architecture.md`
- Sysreg device map: `doc/system/sysregs.md`
- Per-generation instruction costs: `doc/system/instruction-timing.md`;
  where they come from and how they are measured:
  `doc/internals/instruction-timing.md`
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
│   │              #   (+ sdram_line/, the whole-line adapter variant),
│   │              #   usb/ (SIE cells + MAC + host controller), video/,
│   │              #   eth/ (the Ethernet adapter's cells)
│   ├── sim/       # machine_sim + machine_penumbra2_sim, sim devices, memory models
│   └── fpga/      # FPGA helpers (ECP5 PLL package, lint stubs); board tops under <board>/,
│                  #   Xilinx 7-series helpers under xc7/ (MMCM package, lint stubs)
├── microcode/   # microcode.uasm (single source — assemble via uasm.py)
├── rom/         # Boot ROM (C + asm) and its standalone Makefile
├── sim/         # tb_cpu_prog (program runner), sim_console + per-core shims, per-module tbs,
│                #   uart_host.h (a serial host for tbs driving a board top's pins),
│                #   i2c_slave.h (a slave for tbs holding an I2C bus)
├── third_party/ # vendored files under their own licenses (Digilent board XDCs, UberDDR3)
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
- `build_check.sv` — elaboration-time configuration check that stops
  every build tool; use it for any build-time check, never `initial
  assert` or a generate-block `$error`, which sv2v defuses
  (`doc/internals/coding-standards.md`).

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
  set-associative tree-PLRU — `NUM_WAYS` 2 or 4, the gen2 machine
  selects 2 — WT/WnA, atomic line fill, S_PT pass-through hold).
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
  CPU memory port. 2-state FSM with back-to-back BUSY→BUSY
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
  `HIT_LATENCY`-cycle hit pipeline (2 by default; the gen2 machine
  sets 3 to register the read verdict off the L1-fill cone).
  Write-invalidate-on-hit (write-back is a planned phase). Disabled at reset; software enables via
  `WRSYS SYSDEV_L2_CACHE CTRL=1`.
- `busctl.sv` — SYSDEV_BUS sysreg device: RST (sticky) + CFG_EN for
  autoconfig.
- `autoconfig_dev.sv` — config-space regs, cfg daisy chain with
  CFG_EN toggle, dynamic base-address decode.
- `bus_devsel.sv` — combinational address comparator (BASE/SIZE
  parameterized).
- `bus_device_simple.sv` — the handshake of a simple bus device (every
  access resolves in one cycle): one-cycle `o_read`/`o_write` pulses to
  key side effects on, and the registered `o_ack`.
- `bus_response_fabric.sv` — the system bus's response half, one
  instance per machine top: OR of the slaves' acks, read data gated by
  each slave's own ack, and the watchdog that faults an access nothing
  answers (`TIMEOUT_CYCLES`).
- `bus_wait_meter.sv` — the longest wait one slave has made an access
  take, counted as the watchdog counts it; cleared only by
  configuration, so a board can show a slave's margin on LEDs.
- `onchip_ram.sv` — system RAM in block RAM (a simple bus device with
  byte-enabled writes), for a board or bring-up step with no external
  memory.
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
- `i2c/i2c_master.sv` — I2C bus master, one byte per command with a
  START, repeated START or STOP around it where asked, the ACK checked
  or given. One state per quarter-period step, with a table of both
  lines in every state; open-drain pins in positive logic. No clock
  stretching: SCL held low is a bus error. Frees the bus after reset.
  Its command port takes one consumer; sharing it is open work
  (`doc/TODO/hw-devices.md`).
- `eth/eth_crc32.sv` — the Ethernet frame check sequence, a byte per
  cycle: the bare remainder, held reflected so that the check sequence
  is its bytes in wire order. The adapter's receiver and transmitter
  each hold one (`doc/internals/ethernet.md`).
- `eth/eth_mdio.sv` — PHY management master: one clause-22 frame per
  start, MDC divided from the bus clock, a read's bits taken just
  ahead of MDC's rise, the data pin as value and output enable for the
  board to make a pad of. No reset: a frame in flight runs to its end.
- `eth/eth_tx.sv` — the Ethernet transmitter, on the transmit clock:
  reads a queued slot's `LEN` and words through the slot memory's read
  port and puts preamble, bytes, padding and check sequence on the
  seam a byte per enable, twelve byte times apart; steps `TX_CONS` one
  count a cycle, for frames sent, slots passed over and a queue
  discarded.
- `eth/eth_rx.sv` — the Ethernet receiver, on the receive clock: takes
  byte times from the seam, writes a frame into the slot at `RX_PROD`
  as it arrives, and counts it only once it has ended well formed and
  addressed to the station, the slot's first word (`LEN`) written
  last. A frame that finds the ring full is checked unstored and
  reported for `RX_DROP`.
- `sdram/sdram_pkg.sv` — command encoding + chip presets.
- `sdram/sdram_ctrl.sv` — FSM core: init / refresh / ACT/RW/RECOVER.
  `PHY_OUT_LATENCY` / `PHY_IN_LATENCY` parameters absorb
  registering-PHY pipeline.
- `sdram/sdram_bus_adapter.sv` — sync bus ↔ controller req/rsp;
  pipelined cache fills + speculative `addr+4` prefetch on accepted
  reads (depth-2 CDC carries real + spec; 1-bit tag FIFO routes
  responses).
- `sdram/sdram_stack.sv` — adapter → CDC → controller as one unit,
  bus port to PHY command interface; board tops and `sdram_sim`
  instantiate it and supply their own PHY.
- `sdram_line/` — the same two module names built the other way: the
  controller answers a command with a whole cache line and the adapter
  holds it, so the rest of a fill needs no controller traffic and a
  write into the held line drops it. A top picks a stack as a fileset
  (`SDRAM_STACK`, defaulted per top beside its placement seed), so no
  board file names the choice. The CDC, controller and PHYs are shared
  — only the adapter and the stack differ. Which one a top closes
  timing under is a placement question; measure it per top.
- `sdram/sdram_cdc.sv` — depth-2 async FIFO bridge across the
  CPU↔SDRAM clock domains. Up to 2 outstanding, order-preserving;
  Gray pointers via 2-FF synchronizers; data-before-valid CDC.
- `sdram/sdram_phy_sim.sv` — pass-through PHY for Verilator.
- `sdram/sdram_phy_ecp5.sv` — IOB flops + ODDRX1F clock forward.
  `i_clk` is CLKOS @ 0°, `i_clk_sdram` is CLKOS2 @ ~270° (see
  `doc/internals/sdram-controller.md` and `sdram-optimization.md`).
- `ddr3/ddr3_stack.sv` — the line-buffer adapter and `sdram_cdc` ending
  in a DDR3 controller's pipelined-Wishbone port instead of
  `sdram_ctrl`: one 128-bit burst per line, the glue stateless. Drops
  the Wishbone cycle in reset, cancelling a request in flight. Takes
  the line adapter from `sdram_line/`, so a source list names it.

### Simulation glue (`rtl/sim/`)
- `machine_penumbra2_sim.sv` — `machine_penumbra2` + `unified_bus_mem`:
  the gen2 program-runner wrapper (`make test CORE=penumbra2`).
- `unified_bus_mem.sv` — bus-shaped unified ROM/RAM device (region-
  compressed map, simple bus device, `+rom_hex=`).
- `unified_mem.sv` — dual-port flat stand-in for core-level bring-up
  (registered read, no busy; the machine made it spare).
- `machine_sim.sv` — `cpu_core` + `boot_rom` + `simple_mem` +
  `sim_uart` + `machid` + `busctl` + autoconfig SPI. Shared-bus
  via `bus_devsel`, UART IRQ wired.
- `sim_uart.sv` — NS16450-compatible UART for sim (no FIFO).
- `sim_spi.sv` — SPI master + SD card emulator (via `+sdcard=`).
- `sdram_sim.sv` — wraps `sdram_stack` + `phy_sim` + chip model
  into a `simple_mem`-shaped device. Built twice: once from the
  word stack for `tb_sdram_sim`, and once from the line fileset for
  `tb_sdram_line_test` (a `VARIANT_MODULE_TESTS` entry naming the
  forked sources, since the choice is a fileset rather than a
  parameter).
- `i2c_master_test.sv` — `i2c_master` at a 4 MHz clock, so a step is
  ten cycles, for `tb_i2c_master`: a slave model on the two lines and a
  monitor holding the bus to the standard-mode minimums.
- `eth_mdio_test.sv` — `eth_mdio` at a 25 MHz clock, where MDC's period
  is the 400 ns minimum, for `tb_eth_mdio`: a PHY model on the two
  pins and a monitor holding them to the minimums a PHY asks for.
- `sdram_test.sv` — DUT wrapper exposing controller req/rsp for
  `tb_sdram_test`; `BURST_LEN` selects the read burst.
- `sdram_bl8_test.sv` — the same chain at an eight-beat burst, named as
  its own module because Verilator selects a top rather than a
  parameter set (`tb_sdram_bl8_test`).
- `sdram_model.sv` — behavioral SDR DRAM chip (JEDEC command set,
  burst length and write-burst mode read from the mode register,
  sparse storage, protocol checking).
- `simple_mem.sv` — parameterizable sync SRAM model (default 16 MB),
  configurable READ_LATENCY/WRITE_LATENCY.
- `wb_line_mem.sv` — pipelined-Wishbone memory of 16-byte lines with
  random stall and ack latency, standing in for the DDR3 controller:
  dense over its whole address range (512 MiB of host memory at 25
  address bits), stalled for `CALIB_CYCLES` after reset with
  `o_calib_complete` low, as the controller calibrates.
- `ddr3_sim.sv` — `ddr3_stack` on `wb_line_mem`, 512 MiB, in
  `sdram_sim`'s bus shape plus the memory's calibration output; the
  stack takes the machine's reset, the model its own.
- `ddr3/machine_penumbra2_sim.sv` — fork of the gen2 sim wrapper with
  `ddr3_sim` as RAM and the machine held in reset until calibration
  (`SIM_MEM=ddr3`; the directory goes first on the path, like a core
  variant's).
- `ddr3_stack_test.sv` — `ddr3_stack` on `wb_line_mem`, the memory
  reset apart from the stack (`tb_ddr3_stack_test`, a variant module
  test so it gets the line adapter).
- `nexysvideo/nexysvideo_ddr3.sv` — simulation stand-in for the Nexys
  DDR3 controller wrapper, same ports, `wb_line_mem` on its memory
  port: shadows the wrapper by name for the Nexys top's testbench and
  `fpga-lint` (UberDDR3 neither simulates here nor elaborates for lint
  at the board's configuration).
- `nexysvideo/nexysvideo_video_clk.sv` — simulation stand-in for the
  Nexys display clocks, same ports: the serial clock the oscillator,
  the pixel clock a fifth of it, so the serializers keep their 5x
  contract where the MMCM stub would pass one rate through. Shadows
  the MMCM wrapper for the Nexys top's testbench only; `fpga-lint`
  takes the wrapper.

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
  System PLL from the 25 MHz crystal, 32 MB SDRAM (W9825G6KH or
  compatible), real UART (TX+RX), real SPI with SD card (autoconfig),
  USB host on the US2 socket (autoconfig; 60 MHz from CLKOS3,
  single-ended D+/D- on the `usb_fpga_bd` pads), display adapter on
  the GPDI connector (autoconfig; dedicated video PLL, splash preload),
  boot ROM, `btn[1]` reset.
- `ulx3s/ulx3s_penumbra2_probe_top.sv` — gen2 machine timing probe
  (`VARIANT=probe`): `machine_penumbra2` + a BRAM bus memory at
  25 MHz, terminal outputs folded onto the LEDs so synthesis keeps the
  design, IRQ inputs on `btn[2]`/`btn[3]`. The first build with the
  IF2 tag-compare/way-mux path and the L1↔L2 layer in front of nextpnr
  (`make timing BOARD=ulx3s CORE=penumbra2 VARIANT=probe`), not a
  usable machine.
- `nexysvideo/nexysvideo_clk_rst.sv` — clocking and power-on reset
  every Nexys Video top shares: the CPU clock from the 100 MHz
  oscillator through an MMCM (`xc7_mmcm_pkg`, from `CPU_HZ`), and a
  reset held at least 10 ms past MMCM lock and CPU RESET release.
- `xc7/xc7_thermal_guard.sv` — the XADC's over-temperature shutdown,
  armed, in every 7-series top: the part shuts itself down past 85 C
  and never restarts by itself, only when reprogrammed or power-cycled;
  a warning (75 C, clearing at
  70 C) drives led[7]. Clocked off the oscillator, not the MMCM. The
  xc7 flow refuses a bitstream without it (thermal gate in
  `hw/tools/vivado/gates.tcl`). Its DRP port is where a kernel-visible
  sensor device attaches (`doc/TODO/hw-devices.md`).
- `nexysvideo/nexysvideo_penumbra2_top.sv` — gen2 machine on the Nexys
  Video (`BOARD=nexysvideo CORE=penumbra2[_5]`), growing one device per
  bring-up step: boot ROM, UART, the microSD slot over SPI (autoconfig,
  card power off during the reset hold), the display adapter on the
  HDMI output (autoconfig; its own MMCM, an ODDR and a TMDS_33 OBUFDS
  per pair), the audio device in front of the ADAU1761 codec
  (autoconfig; `audio_i2s_out` on the codec's serial clocks, the
  codec's setup over `i2c_master` on open-drain pads, MCLK from
  `nexysvideo_codec_mclk`; `doc/internals/nexysvideo/audio.md`), and
  512 MiB of DDR3 as system RAM through `ddr3_stack`.
  The controller resets only with its clocks' lock; the machine is held
  in reset until it has calibrated, and LEDs show calibration and the
  RAM's longest wait (`bus_wait_meter`). Its boot testbench
  (`make -C hw/rom && make font8x16.hex && make sim
  MOD=nexysvideo_penumbra2_top`) runs the ROM to the monitor through
  the board pins, with the controller's and display clocks' simulation
  stand-ins and a simulated card on the SD pins, reads a frame of
  DVI off the HDMI pins, and plays frames pushed from the monitor into
  a codec model on the I2C and serial-port pins.
- `nexysvideo/nexysvideo_uart_test_top.sv` — console bring-up top
  (`make fpga TOP=nexysvideo_uart_test_top`): banner and echo on the
  UART port, LEDs for lock, reset, a 1 Hz heartbeat, RX activity and
  framing errors. Constraints in
  `hw/constraints/nexysvideo_uart_test_top.xdc`.
- `nexysvideo/nexysvideo_ddr3.sv` — the board's DDR3 behind UberDDR3
  (`hw/third_party/uberddr3/`), at the machine's configuration (x16,
  two lanes, 15 row bits): a Wishbone burst port, UberDDR3's debug
  port, calibration complete. The one file that instantiates
  `ddr3_top`, so the one licensed GPL-3.0-or-later.
- `nexysvideo/nexysvideo_ddr3_clk.sv` — the DDR3 controller's four
  clocks (83.33, 333.33, 333.33 at 90 degrees, 200 MHz) from one PLL.
- `nexysvideo/nexysvideo_video_clk.sv` — the display's pixel and TMDS
  serial clocks (25 and 125 MHz) from an MMCM of their own, off a
  1000 MHz VCO.
- `nexysvideo/nexysvideo_codec_setup.sv` — brings the ADAU1761 codec up
  over I2C after every reset, as a table of steps (one register each):
  a running codec muted before its clock stops, the PLL set and its
  lock polled, the playback path configured, a precharge wait, then
  the unmute. Reports BUSY, FAULT and a fault code naming what failed
  and in which step, for the audio device's STATUS. Testbench
  `tb_nexysvideo_codec_setup` (through `nexysvideo_codec_setup_test`,
  a 1 MHz harness) puts a model of the codec's control port at its
  I2C command port.
- `nexysvideo/nexysvideo_codec_mclk.sv` — the codec's 12.5 MHz master
  clock, the oscillator divided in a counter: no clock manager, and no
  reset, so it runs from configuration on.
- `nexysvideo/nexysvideo_ddr3_selftest.sv` — runs the controller's
  calibration and full-chip self-test over and over, one line per run
  on the console (verdict, reads checked, mismatches, calibration
  time). Testbench `make sim MOD=nexysvideo_ddr3_selftest_test`.
- `nexysvideo/nexysvideo_ddr3_test_top.sv` — DDR3 bring-up top
  (`make fpga TOP=nexysvideo_ddr3_test_top`): the controller and its
  self-test alone, no CPU. Its DDR3 pin lines come from Digilent's MIG
  project (`hw/third_party/digilent-boards/`).

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
`w <addr> <val> [<val>…]` (store words),
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

### Instruction cycle counts feed the compiler
`doc/system/instruction-timing.md` records each instruction's issue
cycles and result latency on each core, and the LLVM scheduling models
(`llvm/llvm/lib/Target/Penumbra/Penumbra.td`, `WriteRes` latencies)
state the same figures; the compiler's cost decisions — constant
multiply/divide strength reduction, the TTI cost model — read them.
**Any RTL change that can move a steady-state cycle count** must update
both in the same change: µop counts in a microcode routine, the fetch
path or a cache's hit latency, the divmul unit's steps or handshake,
pipeline stall, forwarding or writeback sequencing. Re-measure with the
method in `doc/internals/instruction-timing.md`'s "Measuring" section
rather than re-deriving from the RTL; if a change can't be measured
yet, mark the figure as inferred in the reference.

### Device bus contract (`o_ack`)
A bus device completes each access with a registered one-cycle
`o_ack`, never in the cycle the access is presented
(`doc/hardware/bus-protocol.md`, "Slave obligations"). **The master
holds its strobe through the ack cycle, so a device never keys a side
effect on the bare `i_re`/`i_we`** — a held write would commit twice,
a held FIFO read would pop twice.

A device that resolves every access in one cycle instantiates
`soc/bus_device_simple.sv` and keys every side effect on its
`o_read`/`o_write` pulses (see `uart.sv`). A device whose latency
varies (the SDRAM adapter) raises its own ack under the same rules.

### Memory bus
- STALL-based load/store: same microcode regardless of memory
  latency. STALL also checks `i_mem_fault` alongside `i_mem_busy`;
  on fault, sequencer aborts to `S_FETCH`.
- Split I/D L1 caches share the CPU memory port via `cpu_bus_arbiter`
  (D-priority on simultaneous pending); the L2 bridges that stall
  handshake onto the ack-completing system bus.
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

Tools for synthesis, PnR and bitstream packing are
containerized in `tools/oss-cad-suite/`, Docker image based on
Ubuntu 22.04 + OSS CAD Suite release.

Wrapper symlinks in `tools/oss-cad-suite/bin/` (all resolve to
`docker-wrapper.sh` — multi-call pattern): `yosys`, `nextpnr-ecp5`,
`ecppack`, `iverilog`, `vvp`.  The `iverilog`/`vvp` pair
serves the gate-level harness in `sim/gate/`, which needs an event
simulator rather than Verilator. Container runs as host UID and GID
to avoid root-owned output, and has no USB access.

Every board is programmed by `tools/openfpgaloader/bin/openFPGALoader`,
built from a pinned upstream commit into its own image
(`make openfpgaloader-image`; the OSS CAD Suite release cannot parse
current Vivado bitstreams). It needs no `--privileged`: the wrapper
mounts `/dev/bus/usb` and admits only USB device nodes. It opens a
board's FTDI chip by USB ID alone. Don't route a board back through
`fujprog`: when no ULX3S matches by name it falls back to any FT232R,
a Nexys Video's UART included, and drives JTAG into it.

Vivado (Xilinx 7-series boards) lives in `tools/vivado/`, the same
multi-call shape (`bin/vivado`) over a locally built image
(`make vivado-image`). AMD's EULA forbids redistributing it, so the
image is never pushed; it is itself the toolchain's archive, since a
rebuild downloads from AMD again. `image.env` pins the release and tag
for both the Makefile and the wrapper; `smoke.tcl` runs as the image
build's last step, and `build.tcl` is the whole xc7 flow behind
`make fpga` (one Vivado batch run, synthesis to bitstream; see
"Device families" in `doc/internals/build-system.md`). AMD's
installer only ever runs in the Dockerfile's `base` stage, never on
the host. Its per-release files (web installer, login token) live in
`VIVADO_DIR`, outside the repo, and `make vivado-authenticate`
refreshes the week-long token a rebuild needs. The wrapper runs with
no network and gives Vivado a scratch `HOME` under
`build/vivado-home`.

Build flow and `make fpga`/`make flash`/`make timing` commands are
documented in the root `CLAUDE.md`.
