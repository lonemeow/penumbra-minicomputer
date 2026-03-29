# Penumbra Minicomputer - Claude Code Context

## Project Overview
Penumbra is a 32-bit RISC-like minicomputer designed from scratch and implemented on a Radiona ULX3S (Lattice ECP5) FPGA. The project covers the full system: CPU, MMU, DMA, I/O, and system bus. The eventual goal is to port NetBSD, and later build the design from discrete 74xx chips.

## Key Decisions
- **HDL:** SystemVerilog for RTL
- **Toolchain:** Open-source FPGA tools (Yosys, nextpnr-ecp5, Project Trellis)
- **Simulation:** Verilator 5.046 via Docker (`verilator/verilator:latest`), driven by Makefile
- **Target board:** ULX3S with ECP5-85F (32 MB SDRAM, USB, HDMI, GPIO, etc.)
- **OS target:** NetBSD (drives privilege, interrupt, MMU design). See `doc/toolchain/toolchain-strategy.md`
- **Compiler:** LLVM backend (planned). Calling convention and codegen strategy in toolchain doc
- **Discrete build:** All design decisions must be feasible in 74xx discrete logic
- **MUL/DIV/FP strategy:** Unified ALU (no separate long-latency unit). Multi-cycle ops use alu_start/alu_busy. MUL/DIV initially trapped as illegal instructions, SW emulated, hardware added incrementally. FPU follows same pattern with reserved alu_op slots.

## Architecture Summary
The architecture is fully specified in `doc/`. Key specs:
- **ISA:** `doc/isa/architecture-overview.md` — 4-format 32-bit encoding (R/L/M/B), 2-operand, R0=zero, 16 registers, ARM-style condition flags
- **Datapath:** `doc/core/datapath.md` — three-bus (A/B/R), separate PC unit, 51-bit horizontal microcode, hardwired fetch unit, direct-mapped dispatch
- **Microcode reference:** `doc/core/microcode-reference.md` — complete micro-word format, field reference, ROM layout, sequencer behavior, all implemented micro-routines, how to add new instructions
- **Bus:** `doc/bus/bus-overview.md` — custom async Penumbra Bus (4-phase handshake), sync internal bus, sysreg sideband
- **MMU/Cache:** `doc/mmu/mmu-overview.md` — software-managed 64-entry 2-way SA TLB, split I/D PIPT cache, write-through D-cache
- **Sysregs:** `doc/isa/sysregs-reference.md` — programmer's reference for WRSYS/RDSYS: device map, register layouts, TLB packing, assembly recipes

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

### Naming: Hardware vs Software Terminology
- **Supervisor** = hardware privilege level (SR.S bit, CPU mode). Use for anything the CPU implements: supervisor mode, supervisor stack pointer (SSP), "privileged / supervisor-only".
- **Kernel** = OS software running in supervisor mode. Use when referring to the OS: kernel code, kernel handler, kernel pages, kernel-only (TLB access control from the programmer's perspective).
- **Special-purpose registers (SPRs)** = CPU-internal registers accessed via `RDSPR`/`WRSPR` (EPC, ESR, USP). Not device-mapped sysregs. Named from the programmer's perspective, not microarchitecture — e.g. EPC/ESR ("exception PC/SR"), not "shadow PC/SR" (implementation detail). EPC/ESR written only by hardware (exception entry) or restored by `ERET`. USP accessed via `RDSPR Rd, USP` / `WRSPR USP, Rd` using the cross_bank micro-word bit (bit 49) to reach the banked-away user stack pointer from supervisor mode.
- **System registers (sysregs)** = device-mapped registers on the sysreg bus, accessed via `WRSYS`/`RDSYS` (MMU control, TLB entries, system ID). These belong to peripheral devices, not the CPU core.

## Build System
- `make smoke` — toolchain smoke test (trivial adder)
- `make sim MOD=<name>` — build & run a module's Verilator testbench (auto-includes penumbra_pkg.sv, sets --top-module)
- `make sim MOD=machine_sim TB=<tb> PROG=<prog>` — run a specific testbench with a specific program (e.g., `TB=tb_cpu_prog PROG=test_fib`). Auto-assembles `sim/programs/<PROG>.s` and `sw/microcode/microcode.uasm` into hex before running; `.hex` files are build artifacts (gitignored), only `.s`/`.uasm` sources are committed.
- `make test` — run all `sim/programs/test_*.s` programs through machine_sim; builds once, runs each, reports pass/fail summary
- `make wave MOD=<name>` — open VCD waveform in GTKWave
- `make clean` — remove build artifacts
- All simulation runs via Docker (`verilator/verilator:latest`) — no host install needed
- Build artifacts go in `build/`, which is gitignored
- **Important:** Always `rm -rf build/<mod>.verilator build/V<mod>` before rebuilding if you suspect stale binaries (WSL2 /mnt/c filesystem can have stale mtimes)

## Current Status
The CPU runs real programs in simulation with the full CPU → MMU → cache → memory path wired. The TLB is implemented (64-entry 2-way SA, fully software-managed) and tested with MMU enabled (identity + non-identity mappings). MMU data faults (TLB miss, protection violation) are wired as synchronous exceptions — detected during STALL, abort the instruction, save EPC/ESR, and vector to the fault handler. WRSYS/RDSYS access a multi-device sysreg bus (device 0 = MMU, device 1 = SYS). RTL is built bottom-up from leaf modules. The 51-bit micro-word format is finalized (bit [50] = priv, [49] = cross_bank, [1:0] = ei_set/di_set).

### Implemented RTL Modules (all tested)
| Module | File | Tests | Description |
|--------|------|-------|-------------|
| ALU | `rtl/core/alu.sv` | 39/39 | Unified compute unit, 11 single-cycle ops, multi-cycle stubs |
| Register file | `rtl/core/regfile.sv` | 41/41 | 2R/1W, R0=zero, R14 banked USP/SSP, R15→PC, debug port |
| Condition evaluator | `rtl/core/cond_eval.sv` | 256/256 | 16 ARM-style conditions, exhaustively tested |
| Immediate extractor | `rtl/core/imm_ext.sv` | 14/14 | Zero/sign-extend, shift-left-16 |
| Field extractor | `rtl/core/field_ext.sv` | 34/34 | IR → all format fields (R/L/M/B) |
| B-mux | `rtl/core/bmux.sv` | 4/4 | ALU B input: reg/imm/const4/const8 |
| W-mux | `rtl/core/wmux.sv` | 2/2 | Write-back: R-bus or MDR |
| A-bus source mux | `rtl/core/amux.sv` | 4/4 | A-bus: reg/ESR/EPC/vector |
| PC source mux | `rtl/core/pc_mux.sv` | 6/6 | Next PC: hold/+4/+offset/A-bus/MDR |
| Status register | `rtl/core/status_reg.sv` | 64/64 | NZCV flags, S/I mode bits, ESR, ei_shadow |
| PC register | `rtl/core/pc_reg.sv` | 31/31 | PC reg, PC+4 adder, PC+offset adder, EPC |
| MAR | `rtl/core/mar.sv` | 6/6 | Memory address register, loads from R-bus |
| MDR | `rtl/core/mdr.sv` | 7/7 | Memory data register, loads from memory or A-bus |
| Datapath top | `rtl/core/datapath.sv` | 15/15 | Structural wiring of all modules, IR reg, reg addr routing, F-bit gating |
| Microcode ROM | `rtl/core/ucode_rom.sv` | — | 256×51-bit ROM, $readmemh from microcode.hex |
| Sequencer | `rtl/core/sequencer.sv` | — | Micro-PC, branch_cond decode, EI/DI tracking, ei_shadow_clr |
| Byte extractor | `rtl/core/byte_ext.sv` | 19/19 | Sub-word load extraction: byte/half from 32-bit word, sign/zero extend |
| Byte replicator | `rtl/core/byte_rep.sv` | 10/10 | Sub-word store lane positioning: replicate byte/half across all lanes |
| CPU core | `rtl/core/cpu_core.sv` | 20 progs | Full CPU: datapath + sequencer + ROM + MMU + cache + fetch + IRQ + MMU traps + BREAK + SYSCALL + privilege traps + illegal instruction trap + WRSYS/RDSYS + RDSPR/WRSPR + BL + sub-word loads/stores |
| Sim machine | `rtl/soc/machine_sim.sv` | (top) | Simulation integration: cpu_core + simple_mem + sysid. Verilator top module for `make test` |
| Shared package | `rtl/core/penumbra_pkg.sv` | — | REG_*, ALU_*, COND_*, SR_*, ACC_*, VEC_*, SYSDEV_*, SYSREG_* constants |
| System ID | `rtl/soc/sysid.sv` | via machine_sim | Read-only MACHINE_ID register (Penumbra/1), sysreg device 1 |
| TLB | `rtl/mmu/tlb.sv` | 111/111 | 64-entry 2-way SA, parallel lookup, one-hot permission check, indexed sysreg R/W |
| MMU | `rtl/mmu/mmu.sv` | — | Bypass/translate mux, force_bypass for vector fetch, sysreg routing, fault latching, TLB instantiation |
| Cache stub | `rtl/soc/cache_stub.sv` | — | Combinational pass-through with byte_en, placeholder for split I/D PIPT caches |
| Simple memory | `rtl/soc/simple_mem.sv` | — | 4K×32 synchronous SRAM model, $readmemh, 1-cycle read busy, per-byte write enables |

### Exception and Interrupt Handling
Six sources share the same `except_entry` → `int_entry` → vector dispatch path:

**External IRQ (asynchronous):**
- **Check point:** Dispatch-time (when `ir_valid` fires, before entering S_EXEC)
- **Check logic:** `irq_taken = i_irq & sr_i & !ei_shadow` (combinational, safe because sr_i is registered)
- **Action:** Override dispatch to 0x70 (int_entry), pulse `except_entry` (saves EPC/ESR, sets S=1/I=0)
- **EI:** Sets sr_i=1 and ei_shadow=1; ei_shadow cleared after next instruction completes (ei_pending tracking in sequencer)
- **DI:** Sets sr_i=0 immediately; privileged (checked at dispatch, never executes in user mode)

**MMU data fault (synchronous):**
- **Check point:** During STALL on load/store micro-ops (sequencer checks `i_mem_fault`)
- **Detection:** `data_fault = mmu_fault && !fetch_active` — only data accesses, not instruction fetches
- **Action:** `fault_except` pulse (once per fault via `!fault_pending` guard) → `except_entry` saves EPC/ESR, sets S=1/I=0. Sequencer aborts STALL (`go_fetch`), returns to S_FETCH.
- **Dispatch:** `fault_pending` flag overrides next dispatch to 0x70 with `fault_vector` (VEC_TLB_MISS=2 or VEC_TLB_PROT=3). Cleared when int_entry executes (`ctl_pc_load`), one cycle after dispatch — so `vector_num` reads the correct fault vector during int_entry.
- **PC preservation:** PC is in HOLD during STALL, so EPC = faulting instruction. Handler can fill TLB and ERET to restart.
- **Priority:** fault_pending > illegal_pending > priv_pending > BREAK > SYSCALL > IRQ (fault/illegal/priv set SR.I=0, so irq_taken is false at next dispatch)
- **Instruction fetch faults:** Not yet handled — kernel code assumed identity-mapped.

**BREAK instruction (synchronous):**
- **Check point:** Dispatch-time, detected by `dispatch_addr == 0x4A`
- **Action:** Triggers `except_entry` like IRQ, vectors to VEC_BREAK (6). Works from any privilege level.
- **Testbench:** `o_halted` pulses for one cycle at BREAK dispatch — testbench stops immediately. CPU continues with the exception normally (no special halt state).
- **Unprivileged code:** BREAK just traps to the kernel, same as any exception. OS can install a BREAK handler for debugging.

**SYSCALL instruction (synchronous):**
- **Check point:** Dispatch-time, detected by `dispatch_addr == 0x48` (op=20).
- **Action:** Triggers `except_entry` like BREAK, vectors to VEC_SYSCALL (5). Works from any privilege level — its ROM slot is empty (intercepted at dispatch), so the priv bit is never checked.
- **EPC:** Points at the SYSCALL instruction (not the next one). Handler must advance EPC by 4 before returning via `ERET Rd, Rs`.
- **No microcode:** Slot 0x48 is left empty (sentinel fallback). Dispatch redirects to int_entry (0x70).

**Privilege violation (synchronous):**
- **Check point:** First micro-op of S_EXEC. Privileged instructions have `priv=1` in their first micro-word (bit 50). Sequencer detects `priv=1 && !sr_s` and suppresses all enables (`exec_en = executing & !priv_block`), then aborts to S_FETCH.
- **Action:** `priv_except` pulse → `except_entry` saves EPC/ESR, sets S=1/I=0. `priv_pending` overrides next dispatch to 0x70 with VEC_PRIV (4). Cleared at `ctl_pc_load`.
- **PC preservation:** All enables suppressed on the priv-violating micro-op (including pc_load), so EPC = the faulting instruction.
- **Design:** Privilege was originally checked at dispatch time via hardwired address range (`is_sys_zone && !is_priv_exempt`). Moved into the microcode ROM as a `priv` bit — each instruction explicitly declares its privilege level. No address comparators or exception lists needed. In discrete: one AND gate per enable line.

**Illegal instruction (synchronous):**
- **Check point:** First micro-op of S_EXEC. Unused ROM entries are filled with a sentinel (`branch=7`, all other fields zero) by the microcode assembler.
- **Detection:** Sequencer detects `branch == BR_ILLEGAL (3'd7)`, asserts `o_illegal`, and aborts to S_FETCH. cpu_core sets `illegal_pending`.
- **Action:** `illegal_except` pulse → `except_entry` saves EPC/ESR, sets S=1/I=0. `illegal_pending` overrides next dispatch to 0x70 with VEC_ILLEGAL (7). Cleared at `ctl_pc_load` (same timing as fault_pending).
- **PC preservation:** Sentinel has `pc=HOLD` (field=0), so EPC = the illegal instruction. Handler can emulate and ERET with EPC+4, or abort the process.
- **Covers:** All undefined opcodes, reserved Format L/M/B encodings, unimplemented instructions (MUL/DIV/MOD dispatch to sentinel ROM entries).

**Vector table:** Fixed **physical** addresses, MMU bypassed for the vector fetch. `vector_addr = {26'b0, vector_num, 2'b00}` — word-aligned entries at physical 0x00. VEC_RESET=0, VEC_IRQ=1, VEC_TLB_MISS=2, VEC_TLB_PROT=3, VEC_PRIV=4, VEC_SYSCALL=5, VEC_BREAK=6, VEC_ILLEGAL=7. After int_entry completes, `vector_fetch` flag forces MMU bypass for one fetch cycle, cleared on ir_valid. No TLB mapping needed for the vector page — eliminates nested TLB miss on exception entry.

**Dispatch-time vector latching:** Dispatch-time exceptions (BREAK, SYSCALL, IRQ) use `dispatch_pending`/`dispatch_vector` to register the vector number when `ir_valid` fires. This is necessary because `vector_num` is consumed one cycle later by int_entry's `a_src=VECTOR`, but the combinational inputs (`mem_rdata`, `sr_s`) have changed by then — `mem_rdata` reads from MAR (not PC) during S_EXEC, and `sr_s` flips to 1 from `except_entry`. MMU faults already had this pattern via `fault_pending`/`fault_vector`.

**Key bugs found:**
- `ei_pending` clear condition must include `executing` — during S_FETCH, `go_fetch` can be stale from the previous micro-word's ROM output
- `fault_pending` must NOT clear at `ir_valid` (dispatch time) — `vector_num` is read one cycle later during int_entry execution. Clear at `ctl_pc_load` instead.
- **Dispatch-time vector_num was combinational:** `break_taken`/`irq_taken` depend on `mem_rdata` and `sr_s`, which change between dispatch and int_entry. Fixed by registering vector at dispatch time (`dispatch_pending`/`dispatch_vector`). Privilege check was also combinational at dispatch but has since moved to ROM-based `priv` bit (checked by sequencer during S_EXEC).
- **RDSPR unreachable in assembler:** The `RDSPR` handler in pasm.py was nested inside `if mn in FORMAT_R_OPS`, but `"RDSPR"` is not a key (internal keys are `_RDSPR_ESR`/`_RDSPR_EPC`). Code was dead — any test using RDSPR silently ran stale hex. Fixed by moving handler before the guard.

### Register Address Routing
The micro-word's `reg_a_sel`, `reg_b_sel`, `reg_w_sel` fields use a 4-bit encoding:
- `4'b0000` (IR_RD): format-dependent destination register (R→IR[24:21], L→IR[26:23], M→IR[25:22])
- `4'b0001` (IR_RS): format-dependent source/base register (R→IR[20:17], M→IR[21:18])
- `4'b0010–4'b1111`: literal register R2–R15

F-bit write-enable gating only applies when `reg_w_sel = IR_RD` (not for literal addresses).

### Memory Access
- **STALL-based:** Load/store micro-routines use `branch=STALL` to wait for memory. The same microcode works regardless of memory latency (1-cycle sync, cache miss, MMU walk).
- **mem_busy signal:** Simple memory model provides 1-cycle busy for reads, 0-cycle for writes. Future: replaced by cache/bus controller busy signal.
- **MMU traps:** STALL path checks `i_mem_fault` alongside `i_mem_busy`. On fault, sequencer aborts to S_FETCH; `cpu_core` generates `except_entry` and sets `fault_pending` for vector dispatch. PC is in HOLD during STALL, so faulting instruction can be restarted after TLB refill.
- **Dispatch spacing:** Format R uses ×2 spacing split by op[4]: ALU (0x00–0x1E) and SYS (0x40–0x5E). Formula: `{0, op[4], 0, op[3:0], 0}` — pure wiring, zero gates. Format M uses ×4 spacing (0x80–0xBF). Multi-step system ops (ERET, RDSYS) fit in their ×2 slots without overflowing into adjacent instruction entries.

### Software Tools
- **Microcode assembler** (`sw/tools/uasm.py`): Symbolic microcode → $readmemh hex. Defaults: `pc=NEXT branch=FETCH`. Validates slot boundaries (detects multi-step routines that overflow their dispatch slot). Run: `python3 sw/tools/uasm.py input.uasm -o microcode.hex`
- **ISA assembler** (`sw/tools/pasm.py`): Two-pass assembler for Penumbra ISA → $readmemh hex. All 4 formats (R/L/M/B), labels, label references in Format L immediates, pseudo-ops (NOP, RET), branch aliases (BZ/BNZ), `.equ` named constants, built-in sysreg constants (`#MMU`, `#TLB_INDEX`, `#TLB_V`, etc.). Smart mnemonic routing: ADD/SUB/CMP auto-select Format R (reg) or Format L (imm); ERET unifies exception return (0 args = EPC/ESR, 2 args = explicit); RDSPR/WRSPR route to per-SPR opcodes (ESR, EPC, USP). GETUSP/SETUSP accepted as legacy aliases. Run: `python3 sw/tools/pasm.py input.s -o program.hex`
- Makefile auto-assembles `.s`/`.uasm` sources into root-level `program.hex`/`microcode.hex` for `$readmemh`; hex files are build artifacts (gitignored)

### Test Convention
- **Program runner** (`sim/tb_cpu_prog.cpp`): Generic testbench that runs a program until BREAK, checks R1 for pass/fail. VCD trace output to `waves/machine_sim.vcd`, register dump (R0–R15) on failure.
- **Pass/fail convention:** R1 = 1 means PASS, R1 = 0 means FAIL. Tests self-check internally and set R1 accordingly.
- **Halt detection:** Testbench watches for `o_halted` pulse (BREAK instruction dispatch). Instant detection, no polling.
- **Test termination:** Programs end with `BREAK` instruction. Pass path: `LLI R1, #1` then fall through to `fail: BREAK`. Fail path: assertion `BNE fail` branches to `fail: BREAK`.
- **Calling convention:** Return via RET (JMP R13). Program preamble sets LR and calls the test subroutine.

### Implemented Microcode (39 micro-ops)
| Category | Instructions | Notes |
|----------|-------------|-------|
| ALU (R-ALU, 0x00–0x1E) | ADD, SUB, AND, OR, XOR, SHL, SHR, SAR, MOV, NOT | CMP/TEST via F-bit gating on SUB/AND |
| Immediate (Format L) | LLI, LLIS, LUI, ADD #imm, SUB #imm, CMP #imm | Formerly INC/DEC/CMPI (still accepted as aliases) |
| Memory (Format M) | LDW/LDH/LDHS/LDB/LDBS (3 micro-ops), STW/STH/STB (4 micro-ops) | STALL-based, latency-agnostic; byte_ext extracts on load, byte_rep replicates on store, byte_en selects lanes |
| Branch (Format B) | Bcc (all 15 conditions via single BRT entry), BL (2 micro-ops) | BL saves PC+4 to R13, dispatches to 0x62; BZ/BNZ aliases in assembler |
| System (R-SYS, 0x40–0x5E) | JMP, EI, DI, WRSYS, RDSYS, ERET, ERET Rd/Rs, RDSPR, WRSPR, SYSCALL, BREAK | RET = JMP R13 (pseudo); ERET/RDSYS are 2-micro-op; SYSCALL/BREAK intercepted at dispatch; RDSPR/WRSPR use cross_bank for USP |
| Exception | int_entry | Shared by IRQ, MMU fault, BREAK, privilege violation, and illegal instruction dispatch |

### Known Bugs Fixed (Notable)
- **IR corruption from shared mem_rdata bus:** ir_valid lingered one cycle into S_EXEC, causing IR to reload when mem_rdata was muxed to sysreg data. Fix: `ir_load = ir_valid && fetch_active`.
- **BR_PRIV used advance instead of go_fetch:** (Historical — BR_PRIV removed; privilege now checked via microcode priv bit in sequencer.)
- **reg_w_sel gated by executing:** Could change at same posedge as register write. Ungated to keep address stable.
- **fault_pending cleared too early:** Clearing at `ir_valid` (dispatch) meant `vector_num` was wrong one cycle later when `int_entry` read it. Fix: clear at `ctl_pc_load` (int_entry execution).
- **Vector fetch corrupted fault registers:** MMU bypass for vector fetch only gated the output mux, not the TLB lookup or fault latching. The TLB still reported a miss for the unmapped vector page, overwriting `FAULT_ADDR` with the vector address. Fix: gate `i_lookup_en` and fault latching with `!i_force_bypass`.
- **ERET (was IRET) assembled with wrong opcode:** Hardcoded `encode_format_r(31, ...)` instead of using table value (27). Dispatched to wrong ROM entry. Fix: use `op` from FORMAT_R_OPS lookup.

### Next Steps (in priority order)
1. **More system ops** — GETSR/SETSR (if needed).
3. **Timer** — Programmable timer/counter for NetBSD hardclock() scheduler tick.
4. **UART** — Console I/O for first sign of life on real hardware.
5. **Interrupt controller** — Multiple devices with priority encoding.
6. **Instruction fetch faults** — Detect TLB miss during fetch phase (separate from STALL-based data fault path).
7. **Memory subsystem** — Cache (replace cache_stub), SDRAM controller, bus interface.
8. **LLVM backend** — Compiler toolchain for NetBSD port. See `doc/toolchain/toolchain-strategy.md`.
