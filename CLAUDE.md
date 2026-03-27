# Penumbra Minicomputer - Claude Code Context

## Project Overview
Penumbra is a 32-bit RISC-like minicomputer designed from scratch and implemented on a Radiona ULX3S (Lattice ECP5) FPGA. The project covers the full system: CPU, MMU, DMA, I/O, and system bus. The eventual goal is to port Minix 2, and later build the design from discrete 74xx chips.

## Key Decisions
- **HDL:** SystemVerilog for RTL
- **Toolchain:** Open-source FPGA tools (Yosys, nextpnr-ecp5, Project Trellis)
- **Simulation:** Verilator preferred for testbenches
- **Target board:** ULX3S with ECP5-85F (32 MB SDRAM, USB, HDMI, GPIO, etc.)
- **OS target:** Minix 2 (drives privilege, interrupt, MMU design)
- **Discrete build:** All design decisions must be feasible in 74xx discrete logic

## Architecture Summary
The architecture is fully specified in `doc/`. Key specs:
- **ISA:** `doc/isa/architecture-overview.md` — 4-format 32-bit encoding (R/L/M/B), 2-operand, R0=zero, 16 registers, ARM-style condition flags
- **Datapath:** `doc/core/datapath.md` — three-bus (A/B/R), separate PC unit, 48-bit horizontal microcode, hardwired fetch unit, direct-mapped dispatch
- **Microcode validation:** `doc/core/microcode-validation.md` — bit-level micro-programs for ADD, LDW, BEQ, interrupt entry, RTI; 17 issues found and resolved
- **Bus:** `doc/bus/bus-overview.md` — custom async Penumbra Bus (4-phase handshake), sync internal bus, sysreg sideband
- **MMU/Cache:** `doc/mmu/mmu-overview.md` — software-managed 64-entry 2-way SA TLB, split I/D PIPT cache, write-through D-cache

## Repository Layout
- `rtl/` - Synthesizable SystemVerilog, organized by subsystem
- `sim/` - Testbenches and simulation infrastructure
- `sw/` - Assembler, ROM monitor, test programs
- `doc/` - Architecture specs (ISA, MMU, bus, memory map, datapath)
- `constraints/` - ULX3S pin/timing constraints

## Conventions
- RTL filenames match the top-level module they contain
- One module per file
- Use `logic` rather than `reg`/`wire` where possible
- Prefix module ports: `i_` for inputs, `o_` for outputs
- Clock signal: `i_clk`, synchronous active-high reset: `i_rst`

## Current Status
Microcode validation is complete. The 48-bit micro-word format is finalized (revised from the original 52-bit draft through iterative validation). Key architectural improvements discovered during validation: hardwired fetch unit (upgradeable to prefetch), simplified micro-sequencer (no absolute jumps), combined MUL/DIV unit, PRIV check mechanism, page fault handling via STALL extension. Next step is RTL module decomposition and implementation.
