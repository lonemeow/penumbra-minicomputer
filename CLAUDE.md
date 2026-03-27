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
- Clock signal: `i_clk`, synchronous active-high reset: `i_rst` — sampled on rising edge of `i_clk`; while asserted, all state holds reset values (system suspended); testbench holds for 2 cycles then releases
- Shared constants in `rtl/core/penumbra_pkg.sv` (register addresses, ALU opcodes, condition codes)
- Modules that use the package: `import penumbra_pkg::*;` inside the module declaration (not at file scope — Verilator warns about `import *` at $unit scope)

## Build System
- `make smoke` — toolchain smoke test (trivial adder)
- `make sim MOD=<name>` — build & run a module's Verilator testbench (auto-includes penumbra_pkg.sv, sets --top-module)
- `make sim MOD=cpu_top TB=<tb> PROG=<prog>` — run a specific testbench with a specific program (e.g., `TB=tb_cpu_prog PROG=test_fib`)
- `make wave MOD=<name>` — open VCD waveform in GTKWave
- `make clean` — remove build artifacts
- All simulation runs via Docker (`verilator/verilator:latest`) — no host install needed
- Build artifacts go in `build/`, which is gitignored
- **Important:** Always `rm -rf build/<mod>.verilator build/V<mod>` before rebuilding if you suspect stale binaries (WSL2 /mnt/c filesystem can have stale mtimes)

## Current Status
The CPU runs real programs in simulation. A tail-recursive Fibonacci routine (fib(10)=55) executes correctly in 254 cycles. RTL is built bottom-up from leaf modules. The 49-bit micro-word format is finalized (bits [1:0] = ei_set/di_set).

### Implemented RTL Modules (all tested)
| Module | File | Tests | Description |
|--------|------|-------|-------------|
| ALU | `rtl/core/alu.sv` | 39/39 | Unified compute unit, 11 single-cycle ops, multi-cycle stubs |
| Register file | `rtl/core/regfile.sv` | 41/41 | 2R/1W, R0=zero, R14 banked USP/KSP, R15→PC, debug port |
| Condition evaluator | `rtl/core/cond_eval.sv` | 256/256 | 16 ARM-style conditions, exhaustively tested |
| Immediate extractor | `rtl/core/imm_ext.sv` | 14/14 | Zero/sign-extend, shift-left-16 |
| Field extractor | `rtl/core/field_ext.sv` | 34/34 | IR → all format fields (R/L/M/B) |
| B-mux | `rtl/core/bmux.sv` | 4/4 | ALU B input: reg/imm/const4/const8 |
| W-mux | `rtl/core/wmux.sv` | 2/2 | Write-back: R-bus or MDR |
| A-bus source mux | `rtl/core/amux.sv` | 4/4 | A-bus: reg/shadow_SR/shadow_PC/vector |
| PC source mux | `rtl/core/pc_mux.sv` | 6/6 | Next PC: hold/+4/+offset/A-bus/MDR |
| Status register | `rtl/core/status_reg.sv` | 64/64 | NZCV flags, S/I mode bits, shadow SR, ei_shadow |
| PC register | `rtl/core/pc_reg.sv` | 31/31 | PC reg, PC+4 adder, PC+offset adder, shadow PC |
| MAR | `rtl/core/mar.sv` | 6/6 | Memory address register, loads from R-bus |
| MDR | `rtl/core/mdr.sv` | 7/7 | Memory data register, loads from memory or A-bus |
| Datapath top | `rtl/core/datapath.sv` | 15/15 | Structural wiring of all modules, IR reg, reg addr routing, F-bit gating |
| Microcode ROM | `rtl/core/ucode_rom.sv` | — | 256×49-bit ROM, $readmemh from microcode.hex |
| Sequencer | `rtl/core/sequencer.sv` | — | Micro-PC, branch_cond decode, EI/DI tracking, ei_shadow_clr |
| CPU top | `rtl/core/cpu_top.sv` | 25/25 | Full integration: datapath + sequencer + ROM + MMU + cache + memory + fetch + IRQ |
| Shared package | `rtl/core/penumbra_pkg.sv` | — | REG_*, ALU_*, COND_*, SR_*, ACC_*, SYSREG_MMU_* constants |
| MMU | `rtl/mmu/mmu.sv` | — | Bypass mode (M=0): identity map, uncached. Sysreg interface for MMUCR/fault regs |
| Cache stub | `rtl/soc/cache_stub.sv` | — | Combinational pass-through, placeholder for split I/D PIPT caches |
| Simple memory | `rtl/soc/simple_mem.sv` | — | 4K×32 synchronous SRAM model, $readmemh, 1-cycle read busy |

### Interrupt Handling
- **Check point:** Dispatch-time (when `ir_valid` fires, before entering S_EXEC)
- **Check logic:** `irq_taken = i_irq & sr_i & !ei_shadow` (combinational, safe because sr_i is registered)
- **Action:** Override dispatch to 0x70 (int_entry), pulse `except_entry` (saves shadow PC/SR, sets S=1/I=0)
- **EI:** Sets sr_i=1 and ei_shadow=1; ei_shadow cleared after next instruction completes (ei_pending tracking in sequencer)
- **DI:** Sets sr_i=0 immediately; privileged (uses branch=PRIV in microcode)
- **Key bug found:** `ei_pending` clear condition must include `executing` — during S_FETCH, `go_fetch` can be stale from the previous micro-word's ROM output

### Register Address Routing
The micro-word's `reg_a_sel`, `reg_b_sel`, `reg_w_sel` fields use a 4-bit encoding:
- `4'b0000` (IR_RD): format-dependent destination register (R→IR[24:21], L→IR[26:23], M→IR[25:22])
- `4'b0001` (IR_RS): format-dependent source/base register (R→IR[20:17], M→IR[21:18])
- `4'b0010–4'b1111`: literal register R2–R15

F-bit write-enable gating only applies when `reg_w_sel = IR_RD` (not for literal addresses).

### Memory Access
- **STALL-based:** Load/store micro-routines use `branch=STALL` to wait for memory. The same microcode works regardless of memory latency (1-cycle sync, cache miss, MMU walk).
- **mem_busy signal:** Simple memory model provides 1-cycle busy for reads, 0-cycle for writes. Future: replaced by cache/bus controller busy signal.
- **MMU traps (future):** STALL path will check `mem_fault` alongside `mem_busy` for mid-instruction exceptions (page not present, protection). PC is still in HOLD during STALL, so the CPU state is clean for abort+restart.
- **Dispatch spacing:** Format M uses ×4 spacing (0x80–0xBF) to fit multi-step micro-routines (loads: 3 micro-ops, stores: 4 micro-ops).

### Software Tools
- **Microcode assembler** (`sw/tools/uasm.py`): Symbolic microcode → $readmemh hex. Defaults: `pc=NEXT branch=FETCH`. Run: `python3 sw/tools/uasm.py input.uasm -o microcode.hex`
- **ISA assembler** (`sw/tools/pasm.py`): Two-pass assembler for Penumbra ISA → $readmemh hex. All 4 formats (R/L/M/B), labels, label references in Format L immediates, pseudo-ops (NOP, RET), branch aliases (BZ/BNZ). Run: `python3 sw/tools/pasm.py input.s -o program.hex`
- Makefile auto-copies `program.hex` and `microcode.hex` to project root for `$readmemh`

### Test Convention
- **Program runner** (`sim/tb_cpu_prog.cpp`): Generic testbench that runs a program to halt, checks R1. No cycle-by-cycle internal inspection.
- **Calling convention:** Result in R1, return via RET (JMP R13). Program preamble sets LR and calls the test subroutine. Compatible with future boot ROM.
- **Halt detection:** Testbench watches for PC stability (infinite `B .` loop).

### Implemented Microcode (28 micro-ops)
| Category | Instructions | Notes |
|----------|-------------|-------|
| ALU (Format R) | ADD, SUB, AND, OR, XOR, SHL, SHR, SAR, MOV, NOT | CMP/TEST via F-bit gating on SUB/AND |
| Immediate (Format L) | LLI, LLIS, LUI, INC, DEC, CMPI | |
| Memory (Format M) | LDW (3 micro-ops), STW (4 micro-ops) | STALL-based, latency-agnostic |
| Branch (Format B) | All 16 conditions via single BRT entry | BZ/BNZ aliases in assembler |
| System (Format R) | JMP, EI, DI | RET = JMP R13 (pseudo-op) |
| Exception | int_entry | Dispatch-time IRQ check |

### Next Steps (in priority order)
1. **Sub-word loads** — LDH/LDB/LDHS/LDBS (byte/half-word extraction in writeback path).
2. **More system ops** — RTI, SYSCALL, GETSR/SETSR, GETUSP/SETUSP.
3. **BL (branch-and-link)** — Needs special handling to save PC+4 to LR; all branches currently share one dispatch entry.
4. **Memory subsystem** — Cache, bus interface, MMU for real hardware.
