# Penumbra Minicomputer - Claude Code Context

## Project Overview
Penumbra is a 32-bit RISC-like minicomputer designed from scratch and implemented on a Radiona ULX3S (Lattice ECP5) FPGA. The project covers the full system: CPU, MMU, DMA, I/O, and system bus. The eventual goal is to port Minix 2, and later build the design from discrete 74xx chips.

## Key Decisions
- **HDL:** SystemVerilog for RTL
- **Toolchain:** Open-source FPGA tools (Yosys, nextpnr-ecp5, Project Trellis)
- **Simulation:** Verilator 5.046 via Docker (`verilator/verilator:latest`), driven by Makefile
- **Target board:** ULX3S with ECP5-85F (32 MB SDRAM, USB, HDMI, GPIO, etc.)
- **OS target:** Minix 2 (drives privilege, interrupt, MMU design)
- **Discrete build:** All design decisions must be feasible in 74xx discrete logic
- **MUL/DIV/FP strategy:** Unified ALU (no separate long-latency unit). Multi-cycle ops use alu_start/alu_busy. MUL/DIV initially trapped as illegal instructions, SW emulated, hardware added incrementally. FPU follows same pattern with reserved alu_op slots.

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
- Shared constants in `rtl/core/penumbra_pkg.sv` (register addresses, ALU opcodes, condition codes)
- Modules that use the package: `import penumbra_pkg::*;` inside the module declaration (not at file scope — Verilator warns about `import *` at $unit scope)

## Build System
- `make smoke` — toolchain smoke test (trivial adder)
- `make sim MOD=<name>` — build & run a module's Verilator testbench (auto-includes penumbra_pkg.sv, sets --top-module)
- `make wave MOD=<name>` — open VCD waveform in GTKWave
- `make clean` — remove build artifacts
- All simulation runs via Docker (`verilator/verilator:latest`) — no host install needed
- Build artifacts go in `build/`, which is gitignored
- **Important:** Always `rm -rf build/<mod>.verilator build/V<mod>` before rebuilding if you suspect stale binaries (WSL2 /mnt/c filesystem can have stale mtimes)

## Current Status
RTL implementation is in progress, bottom-up from leaf modules. Microcode validation is complete. The 48-bit micro-word format is finalized. The separate long-latency unit has been folded into a unified ALU (alu_op expanded to 5 bits, lu_op replaced with alu_start).

### Implemented RTL Modules (all tested)
| Module | File | Tests | Description |
|--------|------|-------|-------------|
| ALU | `rtl/core/alu.sv` | 39/39 | Unified compute unit, 11 single-cycle ops, multi-cycle stubs |
| Register file | `rtl/core/regfile.sv` | 41/41 | 2R/1W, R0=zero, R14 banked USP/KSP, R15→PC |
| Condition evaluator | `rtl/core/cond_eval.sv` | 256/256 | 16 ARM-style conditions, exhaustively tested |
| Immediate extractor | `rtl/core/imm_ext.sv` | 14/14 | Zero/sign-extend, shift-left-16 |
| Field extractor | `rtl/core/field_ext.sv` | 34/34 | IR → all format fields (R/L/M/B) |
| B-mux | `rtl/core/bmux.sv` | 4/4 | ALU B input: reg/imm/const4/const8 |
| W-mux | `rtl/core/wmux.sv` | 2/2 | Write-back: R-bus or MDR |
| A-bus source mux | `rtl/core/amux.sv` | 4/4 | A-bus: reg/shadow_SR/shadow_PC/vector |
| PC source mux | `rtl/core/pc_mux.sv` | 6/6 | Next PC: hold/+4/+offset/A-bus/MDR |
| Shared package | `rtl/core/penumbra_pkg.sv` | — | REG_*, ALU_*, COND_* constants |

### Next Steps (in priority order)
1. **Status Register (SR)** — latches ALU flags (NZCV), holds mode bits (S=supervisor, I=interrupt enable). Connects ALU → condition evaluator. Key for exception model.
2. **PC Register + PC Adder** — sequential PC with adder for PC+4 and PC+offset. Feeds PC mux, I-cache address.
3. **MAR / MDR registers** — simple latches bridging datapath to memory subsystem.
4. **Datapath top module** — wire all modules together, integrate field extractor → register file address routing (mux between IR fields and micro-word literal addresses).
5. **Microcode ROM** — 256×48-bit ROM with dispatch logic.
6. **Micro-sequencer** — micro-PC counter with branch_cond control.
7. **Fetch unit** — hardwired instruction fetch, exception dispatch.
