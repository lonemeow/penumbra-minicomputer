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
- **Byte order:** Little-endian. `addr[1:0]=00` maps to bits `[7:0]` (defined by `byte_ext`/`byte_rep`). Matches x86/RISC-V/ARM-LE. The assembler's `.asciz`/`.byte` directives pack data in this order.
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
- `hw/` - All hardware design
  - `hw/rtl/` - Synthesizable SystemVerilog, organized by subsystem
  - `hw/sim/` - Testbenches and test programs
  - `hw/microcode/` - Microcode source (assembled into ROM)
  - `hw/rom/` - Boot ROM firmware
  - `hw/tools/` - Microcode assembler (`uasm.py`)
  - `hw/constraints/` - ULX3S pin/timing constraints (`ulx3s_v20.lpf` — covers all board revisions)
- `sw/` - Software tools
  - `sw/tools/` - ISA assembler (`pasm.py`)
- `doc/` - Architecture specs (ISA, MMU, bus, memory map, datapath, toolchain)

## Conventions
- RTL filenames match the top-level module they contain
- One module per file
- Use `logic` rather than `reg`/`wire` where possible
- Prefix module ports: `i_` for inputs, `o_` for outputs
- Clock signal: `i_clk`, synchronous active-high reset: `i_rst` — sampled on rising edge of `i_clk`; while asserted, all state holds reset values (system suspended); testbench holds for 2 cycles then releases
- Shared constants in `hw/rtl/core/penumbra_pkg.sv` (register addresses, ALU opcodes, condition codes)
- Modules that use the package: `import penumbra_pkg::*;` inside the module declaration (not at file scope — Verilator warns about `import *` at $unit scope)

### Naming: Hardware vs Software Terminology
- **Supervisor** = hardware privilege level (SR.S bit, CPU mode). Use for anything the CPU implements: supervisor mode, supervisor stack pointer (SSP), "privileged / supervisor-only".
- **Kernel** = OS software running in supervisor mode. Use when referring to the OS: kernel code, kernel handler, kernel pages, kernel-only (TLB access control from the programmer's perspective).
- **Special-purpose registers (SPRs)** = CPU-internal registers accessed via unified `RDSPR`/`WRSPR` instructions (EPC, ESR, USP). Not device-mapped sysregs. Named from the programmer's perspective, not microarchitecture — e.g. EPC/ESR ("exception PC/SR"), not "shadow PC/SR" (implementation detail). EPC/ESR written by hardware (exception entry) or by software via `WRSPR EPC`/`WRSPR ESR` (e.g., to skip a faulting instruction or context switch). USP accessed via `RDSPR Rd, USP` / `WRSPR USP, Rd`. All SPR instructions encode the SPR number in IR[15:12] (same position as `sys_dev`), decoded by combinational logic in the datapath.
- **System registers (sysregs)** = device-mapped registers on the sysreg bus, accessed via `WRSYS`/`RDSYS` (MMU control, TLB entries, system ID). These belong to peripheral devices, not the CPU core.

## Build System
- `make smoke` — toolchain smoke test (trivial adder)
- `make sim MOD=<name>` — build & run a module's Verilator testbench (auto-includes penumbra_pkg.sv, sets --top-module)
- `make sim MOD=machine_sim TB=<tb> PROG=<prog>` — run a specific testbench with a specific program (e.g., `TB=tb_cpu_prog PROG=test_fib`). Auto-assembles `hw/sim/programs/<PROG>.s` and `hw/microcode/microcode.uasm` into hex before running; `.hex` files are build artifacts (gitignored), only `.s`/`.uasm` sources are committed.
- `make test` — run all `hw/sim/programs/test_*.s` programs through machine_sim; builds once, runs each, reports pass/fail summary
- `make simulate` — build & run interactive boot ROM with terminal I/O. Assembles `hw/rom/boot_rom.s`, bridges stdin/stdout to UART RX/TX via `tb_interactive.cpp`. Docker runs with `-it` for raw terminal passthrough. No cycle limit, no VCD trace. Exit with Ctrl-C or BREAK.
- `make wave MOD=<name>` — open VCD waveform in GTKWave
- `make clean` — remove build artifacts
- All simulation runs via Docker (`verilator/verilator:latest`) — no host install needed
- Build artifacts go in `build/`, which is gitignored
- **Important:** Always `rm -rf build/<mod>.verilator build/V<mod>` before rebuilding if you suspect stale binaries (WSL2 /mnt/c filesystem can have stale mtimes)

## Current Status
The CPU runs real programs in simulation with the full CPU → MMU → split I/D cache → memory path wired, booting from ROM at `0xFFFF_E000` (matching the physical memory map). Instruction fetch is busy-aware and latency-agnostic (waits for memory busy to deassert, tested with both 1-cycle ROM and 6-cycle SDRAM-latency RAM). RAM simulation models realistic SDRAM timing (READ_LATENCY=6, WRITE_LATENCY=3). Split I/D caches are wired into cpu_core — the I-cache serves instruction fetch, the D-cache serves data loads/stores. A memory bus mux merges both caches to the single external memory port (D-cache priority; safe because fetch and data access are mutually exclusive). Both caches are disabled at reset (pass-through); the kernel enables them via WRSYS after setting up TLB mappings with C=1. Cache sysregs (devices 2/3) are handled internally in cpu_core alongside the MMU (device 0). The simulator (machine_sim) uses a shared-bus model with device-side address decode: each device (RAM, ROM, UART) has a `bus_devsel` comparator that recognizes its own address range, and bus responses are OR-combined (FPGA equivalent of tri-state on a discrete backplane). Unmapped addresses trigger a **bus fault exception** (vector 0, `VEC_BUS_FAULT`) — the bus_fault signal is wired from machine_sim into cpu_core, which treats it like an MMU fault but with a different vector. Bus faults work in both bypass mode (RAM probing at boot) and with MMU enabled (device probing via mapped-but-unmapped pages, like NetBSD `bus_space_peek`). The MMU latches FAULT_ADDR (virtual address) and FAULT_STATUS (type=FAULT_BUS) on bus fault. RAM claims only its actual size (16 MB, no wrapping) — OS probes for RAM size by detecting bus faults. A 16450-compatible simulation UART provides console I/O — TX bytes appear on stdout, RX accepts bytes from the testbench. The UART is memory-mapped (MMIO at `0xFF00_0000`, not on the sysreg bus), accessed via LDW/STW, with realistic TX busy timing (~2170 cycles at 115200 baud/25 MHz). An interactive boot ROM (`hw/rom/boot_rom.s`) prints a banner and runs an echo loop over UART; `make simulate` launches it with terminal I/O bridged through Docker (`-it`). The vector table uses MIPS/68k-style address-based dispatch: `int_entry` reads a handler address from physical RAM, bypassing the MMU. The TLB is implemented (64-entry 2-way SA, fully software-managed) and tested with MMU enabled (identity + non-identity mappings). MMU faults (TLB miss, protection violation) are wired as synchronous exceptions for both data accesses and instruction fetches — data faults detected during STALL, fetch faults detected during S_FETCH — both abort the instruction, save EPC/ESR, and vector to the fault handler. WRSYS/RDSYS access a multi-device sysreg bus (device 0 = MMU, device 1 = SYS, device 2 = DCACHE, device 3 = ICACHE). RTL is built bottom-up from leaf modules. The 51-bit micro-word format is finalized (bit [50] = priv, [49:47] = a_src (3-bit, includes SPR decode), [10:9] = sys_op (NONE/SPR_WRITE/SYS_READ/SYS_WRITE), [1:0] = ei_set/di_set).

### Implemented RTL Modules (all tested)
| Module | File | Tests | Description |
|--------|------|-------|-------------|
| ALU | `hw/rtl/core/alu.sv` | 39/39 | Unified compute unit, 11 single-cycle ops, multi-cycle stubs |
| Register file | `hw/rtl/core/regfile.sv` | 41/41 | 2R/1W, R0=zero, R14 banked USP/SSP, R15→PC, debug port |
| Condition evaluator | `hw/rtl/core/cond_eval.sv` | 256/256 | 16 ARM-style conditions, exhaustively tested |
| Immediate extractor | `hw/rtl/core/imm_ext.sv` | 14/14 | Zero/sign-extend, shift-left-16 |
| Field extractor | `hw/rtl/core/field_ext.sv` | 34/34 | IR → all format fields (R/L/M/B) |
| B-mux | `hw/rtl/core/bmux.sv` | 4/4 | ALU B input: reg/imm/const4/const8 |
| W-mux | `hw/rtl/core/wmux.sv` | 2/2 | Write-back: R-bus or MDR |
| A-bus source mux | `hw/rtl/core/amux.sv` | 4/4 | A-bus: reg/ESR/EPC/vector |
| PC source mux | `hw/rtl/core/pc_mux.sv` | 6/6 | Next PC: hold/+4/+offset/A-bus/MDR |
| Status register | `hw/rtl/core/status_reg.sv` | 64/64 | NZCV flags, S/I mode bits, ESR, ei_shadow |
| PC register | `hw/rtl/core/pc_reg.sv` | 31/31 | PC reg (parameterizable RESET_PC), PC+4 adder, PC+offset adder, EPC |
| MAR | `hw/rtl/core/mar.sv` | 6/6 | Memory address register, loads from R-bus |
| MDR | `hw/rtl/core/mdr.sv` | 7/7 | Memory data register, loads from memory or A-bus |
| Datapath top | `hw/rtl/core/datapath.sv` | 15/15 | Structural wiring of all modules, IR reg, reg addr routing, F-bit gating |
| Microcode ROM | `hw/rtl/core/ucode_rom.sv` | — | 256×51-bit ROM, $readmemh from microcode.hex |
| Sequencer | `hw/rtl/core/sequencer.sv` | — | Micro-PC, branch_cond decode, EI/DI tracking, ei_shadow_clr |
| Byte extractor | `hw/rtl/core/byte_ext.sv` | 19/19 | Sub-word load extraction: byte/half from 32-bit word, sign/zero extend |
| Byte replicator | `hw/rtl/core/byte_rep.sv` | 10/10 | Sub-word store lane positioning: replicate byte/half across all lanes |
| CPU core | `hw/rtl/core/cpu_core.sv` | 30 progs | Full CPU: datapath + sequencer + ROM + MMU + split I/D cache + memory bus mux + fetch + IRQ + MMU traps (data + fetch) + alignment faults (fetch + data, via MMU) + bus faults (via i_bus_fault from machine-level) + BREAK + SYSCALL + privilege traps + illegal instruction trap + WRSYS/RDSYS + RDSPR/WRSPR + BL + sub-word loads/stores. Cache sysregs (devices 2–3) handled internally. Parameterizable RESET_PC (default 0xFFFF_E000). |
| Sim machine | `hw/rtl/soc/machine_sim.sv` | (top) | Simulation integration: cpu_core + boot_rom + simple_mem + sim_uart + sysid. Shared-bus model with device-side address decode via `bus_devsel` — each device self-selects, responses OR-combined, unmapped addresses trigger bus fault exception (wired to cpu_core.i_bus_fault). UART IRQ wired to CPU. Verilator top module for `make test` |
| Bus devsel | `hw/rtl/soc/bus_devsel.sv` | via machine_sim | Combinational address comparator for device-side bus decode. Parameterized BASE/SIZE, elaboration-time assertions (power-of-2, alignment, non-zero). Discrete 74xx equivalent: 74x85 magnitude comparator per device. |
| Sim UART | `hw/rtl/soc/sim_uart.sv` | via machine_sim | 16450-compatible UART (MMIO at 0xFF00_0000). 8 registers at word stride, DLAB mux, TX busy counter (parameterizable, default ~115200 baud at 25 MHz). NetBSD com(4) compatible via reg-shift=2, reg-io-width=4 |
| Boot ROM | `hw/rtl/soc/boot_rom.sv` | via machine_sim | Read-only memory (8 KB default), loads program.hex, same 1-cycle busy protocol as simple_mem |
| Shared package | `hw/rtl/core/penumbra_pkg.sv` | — | REG_*, ALU_*, COND_*, SR_*, ACC_*, VEC_*, FAULT_*, FSTAT_*, SYSDEV_*, SYSREG_*, SYSREG_CACHE_*, CACHE_TYPE_*, UART_*, SPR_*, RAM_BASE, ROM_BASE constants |
| System ID | `hw/rtl/soc/sysid.sv` | via machine_sim | Read-only MACHINE_ID register (Penumbra/1), sysreg device 1 |
| TLB | `hw/rtl/mmu/tlb.sv` | 111/111 | 64-entry 2-way SA, parallel lookup, one-hot permission check, indexed sysreg R/W |
| MMU | `hw/rtl/mmu/mmu.sv` | — | Bypass/translate mux, force_bypass for vector table read, alignment check (word/half/byte via i_mem_size), sysreg routing, fault latching (TLB miss/prot, alignment, bus fault via i_bus_fault), TLB instantiation |
| Cache | `hw/rtl/soc/cache.sv` | 42/42 | Parameterized PIPT cache (NUM_SETS, LINE_WORDS, NUM_WAYS). Write-through/write-no-allocate. Burst line fill on read miss. Sysreg interface (INFO/CTRL/INVAL). Pass-through when disabled (reset default) or uncacheable (C=0). Reusable for both I-cache and D-cache. |
| Cache stub | `hw/rtl/soc/cache_stub.sv` | — | Combinational pass-through, retained for reference. Replaced by cache.sv instances in cpu_core. |
| Simple memory | `hw/rtl/soc/simple_mem.sv` | — | Parameterizable synchronous SRAM model (default 16 MB), zeroed at init (no preload — matches real HW), configurable READ_LATENCY (default 6) and WRITE_LATENCY (default 3) modeling SDRAM timing, per-byte write enables. Bus decode limits addresses to RAM's actual range (no wrapping — unmapped addresses are bus faults). Latches address/data at access start. Read data poisoned (0xDEAD_BEEF) while busy. |

### Boot ROM and Interactive Simulation
- **Boot ROM** (`hw/rom/boot_rom.s`): Penumbra/1 boot monitor with command parser. Identical binary on sim and real hardware. Commands: `d ADDR` (dump 64 bytes), `w ADDR VAL` (write word), `g ADDR` (jump), `?` (help). Stack-based calling convention: SP (R14) at `0x01000000` (top of 16 MB RAM), non-leaf functions push/pop LR. I/O globals pinned in R10-R12 (UART base, THRE mask, DR mask). Routines: `putchar`/`getchar` (leaf, polling), `puts` (null-terminated string), `readline` (line editing with echo and backspace), `parse_hex` (hex string→value), `print_hex32`/`print_hex8`/`print_nibble` (value→hex output, table-driven). Line buffer at RAM `0x1000`, 79-char max. Assembled with `--org 0xFFFFE000`.
- **Interactive testbench** (`hw/sim/tb_interactive.cpp`): Bridges host stdin/stdout to UART RX/TX. Raw terminal mode (no echo, no line buffering — boot ROM handles character processing). Polls stdin every 1024 cycles for sub-character-time latency. UART RX handshake: checks `o_uart_rx_ack` on negedge (combinational, pre-posedge) to reliably detect acceptance. No VCD tracing (interactive sessions are long). Exits on BREAK or SIGINT (Ctrl-C). Status messages go to stderr.

### Exception and Interrupt Handling
Eight sources share the same `except_entry` → `int_entry` → vector dispatch path:

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

**MMU instruction fetch fault (synchronous):**
- **Check point:** During S_FETCH, when MMU reports a fault for the PC address
- **Detection:** `fetch_fault = mmu_fault && fetch_active` — complements data_fault. Both feed into `fault_except`.
- **I-cache gate:** `i_re = fetch_active && !mmu_fault` — prevents the I-cache from reading at a bogus/unauthorized physical address on TLB miss or protection violation.
- **IR protection:** `ir_load` gated by `!fault_pending` — prevents garbage from stale `mem_rdata` being loaded into IR. `ir_valid` still fires (after one-cycle `fetch_pending` delay) to transition the sequencer to S_EXEC with `effective_dispatch = 0x70`.
- **Dispatch-time gate:** `break_taken` and `syscall_taken` gated by `!fault_pending` — stale `mem_rdata` during a fetch fault could produce any `dispatch_addr`, including 0x4A (BREAK) or 0x48 (SYSCALL). Without this gate, spurious `except_entry` would corrupt EPC/ESR, and `o_halted` would stop the testbench.
- **PC preservation:** PC hasn't advanced (still in S_FETCH), so EPC = faulting PC. Handler fills TLB and ERETs to retry the fetch.
- **Timing:** 2-cycle path, same as data faults — cycle N: `fault_except` → `except_entry` (saves EPC/ESR); cycle N+1: `fault_pending` overrides dispatch to int_entry (0x70).

**Alignment fault (synchronous, fetch or data):**
- **Check point:** MMU checks alignment on every request via `i_mem_size`: word requires `addr[1:0]==0`, half requires `addr[0]==0`, byte always OK. Fires even in bypass mode (MMU disabled).
- **Detection:** Fetch: misaligned PC (via JMP Rs, ERET, or corrupted vector table). Data: misaligned LDW/STW/LDH/STH address.
- **Priority:** Highest fault — alignment is checked before TLB lookup. On misalignment, `o_fault=1` and `o_align=1`; TLB is not consulted.
- **Vector:** VEC_ALIGN=8 (address 0x20). `fault_vector` set to `VEC_ALIGN` when `mmu_align` is true.
- **FAULT_ADDR/STATUS:** Latched by MMU like TLB faults. `FAULT_STATUS = {user_mode, access_type, 4'b0, FAULT_ALIGN(3)}`. Access type distinguishes code (ACC_EXEC=0x400) vs data (ACC_READ=0x100, ACC_WRITE=0x200).
- **PC preservation:** Fetch: EPC = misaligned PC. Data: EPC = faulting load/store instruction (PC in HOLD during STALL). Fetch handler diagnoses or terminates; data handler must advance EPC+4 to skip (alignment can't be "fixed").

**Bus fault (synchronous, fetch or data):**
- **Check point:** Physical bus level — fires when a memory request (from cache miss or uncached access) hits no device. Detected combinationally in `machine_sim` as `(mem_re | mem_we) & ~(ram_sel | rom_sel | uart_sel)`, wired to `cpu_core.i_bus_fault`.
- **Timing difference from MMU faults:** MMU faults fire upstream (before cache), gating the access. Bus faults fire downstream (after cache), so the request has already gone out. Cannot gate `data_re`/`data_we` with `i_bus_fault` or it creates a combinational loop. Instead, the request completes with no device responding (rdata=0, busy=0), and the fault is caught in the exception path.
- **Detection:** Combined with `mmu_fault` in cpu_core: `data_fault = (mmu_fault || i_bus_fault) && !fetch_active`, `fetch_fault = (mmu_fault || i_bus_fault) && fetch_active`.
- **Vector:** VEC_BUS_FAULT=0 (address 0x00). Reuses the former reset vector slot (reset is hardwired to RESET_PC, never uses the vector table). Bus fault has highest priority in `fault_vector` selection (checked before alignment, TLB prot, TLB miss).
- **FAULT_ADDR/STATUS:** Latched by MMU on `i_bus_fault` (gated by `!i_force_bypass`). `FAULT_STATUS = {user_mode, access_type, 4'b0, FAULT_BUS(4)}`. FAULT_ADDR contains the *virtual* address (what the software asked for), not the physical address the bus saw.
- **Use cases:** RAM probing at boot (bypass mode — load from addresses above RAM until bus fault); device probing with MMU enabled (map a page to potential device MMIO, attempt read, catch bus fault — the NetBSD `bus_space_peek` pattern).
- **PC preservation:** Same as other faults — EPC = faulting instruction. Handler must advance EPC+4 to skip (bus fault can't be "fixed" by retrying).

**BREAK instruction (synchronous):**
- **Check point:** Dispatch-time, detected by `dispatch_addr == 0x4A`
- **Action:** Triggers `except_entry` like IRQ, vectors to VEC_BREAK (6). Works from any privilege level.
- **Testbench:** `o_halted` pulses for one cycle at BREAK dispatch — testbench stops immediately. CPU continues with the exception normally (no special halt state).
- **Unprivileged code:** BREAK just traps to the kernel, same as any exception. OS can install a BREAK handler for debugging.

**SYSCALL instruction (synchronous):**
- **Check point:** Dispatch-time, detected by `dispatch_addr == 0x48` (op=20).
- **Action:** Triggers `except_entry` like BREAK, vectors to VEC_SYSCALL (5). Works from any privilege level — its ROM slot is empty (intercepted at dispatch), so the priv bit is never checked.
- **EPC:** Points at the SYSCALL instruction (not the next one). Handler must advance EPC by 4 before returning: `RDSPR Rd, EPC; ADD Rd, #4; WRSPR EPC, Rd; ERET`.
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

**Vector table (MIPS/68k-style, address-based):** The vector table at physical 0x00 contains **handler addresses** (not instructions). `int_entry` (3 micro-ops at 0x70–0x72) reads the handler address from `vector_addr = {26'b0, vector_num, 2'b00}`, then loads it into PC via MDR. The vector table data read bypasses the MMU via `vector_read` flag (set on `except_entry`, cleared on `fetch_go`). VEC_BUS_FAULT=0, VEC_IRQ=1, VEC_TLB_MISS=2, VEC_TLB_PROT=3, VEC_PRIV=4, VEC_SYSCALL=5, VEC_BREAK=6, VEC_ILLEGAL=7, VEC_ALIGN=8. Software writes handler addresses to RAM at boot time via `LA Rd, #handler` + `STW Rd, [R0 + #offset]`. No TLB mapping needed for the vector page — eliminates nested TLB miss on exception entry.

**Reset vector:** CPU boots at `RESET_PC` (default `0xFFFF_E000`, parameterizable). This is NOT part of the vector table — it's a hardwired PC reset value. Vector 0 is used for bus faults (reset doesn't go through int_entry). `machine_sim` overrides to default; future `machine_ulx3s` uses the default for ROM boot.

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
- **mem_busy signal:** Simple memory model provides configurable latency (READ_LATENCY=6, WRITE_LATENCY=3 by default, modeling SDRAM). Split I/D caches sit between CPU and memory — I-cache for instruction fetch, D-cache for data loads/stores. Cache hits return in zero latency; misses burst-fill entire lines. A memory bus mux merges both caches to the single external port (D-cache priority; fetch and data are mutually exclusive). Read data poisoned (0xDEAD_BEEF) while busy to catch premature sampling. Faulting data accesses are gated with `!mmu_fault` to prevent stale writes to memory.
- **MMU traps:** STALL path checks `i_mem_fault` alongside `i_mem_busy`. On fault, sequencer aborts to S_FETCH; `cpu_core` generates `except_entry` and sets `fault_pending` for vector dispatch. PC is in HOLD during STALL, so faulting instruction can be restarted after TLB refill.
- **Dispatch spacing:** Format R uses ×2 spacing split by op[4]: ALU (0x00–0x1E) and SYS (0x40–0x5E). Formula: `{0, op[4], 0, op[3:0], 0}` — pure wiring, zero gates. Format M uses ×4 spacing (0x80–0xBF). Multi-step system ops (ERET, RDSYS) fit in their ×2 slots without overflowing into adjacent instruction entries.

### Software Tools
- **Microcode assembler** (`hw/tools/uasm.py`): Symbolic microcode → $readmemh hex. Defaults: `pc=NEXT branch=FETCH`. Validates slot boundaries (detects multi-step routines that overflow their dispatch slot). Run: `python3 hw/tools/uasm.py input.uasm -o microcode.hex`
- **ISA assembler** (`sw/tools/pasm.py`): Two-pass assembler for Penumbra ISA → $readmemh hex. All 4 formats (R/L/M/B), labels, label references in Format L immediates, pseudo-ops (NOP, RET, LA, LI), branch aliases (BZ/BNZ), `.equ` named constants, built-in constants (`#MMU`, `#TLB_INDEX`, `#TLB_V`, `#DCACHE`, `#ICACHE`, `#CACHE_INFO`, `#CACHE_CTRL`, `#CACHE_INVAL`, `#UART_BASE`, `#UART_LSR`, `#LSR_THRE`, `#FAULT_BUS`, `#FAULT_TLB_MISS`, `#FSTAT_R`, `#FSTAT_W`, `#FSTAT_X`, `#FSTAT_USR`, etc.). Smart mnemonic routing: ADD/SUB/CMP auto-select Format R (reg) or Format L (imm); ERET is 0-arg only (use WRSPR EPC/ESR to modify return state); RDSPR/WRSPR are unified instructions with SPR name (ESR, EPC, USP) encoded in spare[15:12]. `LA Rd, #label` loads a full 32-bit label address (LLI+LUI). `LI Rd, #value` loads an arbitrary 32-bit immediate (LLI+LUI). `--org ADDR` sets code base address (default 0). Data directives: `.word` (one or more 32-bit values), `.byte` (one or more bytes, packed little-endian into words), `.asciz "string"` (null-terminated ASCII with C escape sequences: `\n \r \t \\ \" \0 \xNN`). Bytes are packed little-endian so that sequential LDB reads match string order. Run: `python3 sw/tools/pasm.py --org 0xFFFFE000 input.s -o program.hex`
- Makefile auto-assembles `.s`/`.uasm` sources into root-level `program.hex`/`microcode.hex` for `$readmemh`; hex files are build artifacts (gitignored). Programs assembled with `--org 0xFFFFE000` (boot ROM address).

### Test Convention
- **Program runner** (`hw/sim/tb_cpu_prog.cpp`): Generic testbench that runs a program until BREAK (500000 cycle limit), checks R1 for pass/fail. VCD trace output to `waves/machine_sim.vcd`, register dump (R0–R15) on failure. UART TX bytes printed to stdout in real time.
- **Pass/fail convention:** R1 = 1 means PASS, R1 = 0 means FAIL. Tests self-check internally and set R1 accordingly.
- **Halt detection:** Testbench watches for `o_halted` pulse (BREAK instruction dispatch). Instant detection, no polling.
- **Test termination:** Programs end with `BREAK` instruction. Pass path: `LLI R1, #1` then fall through to `fail: BREAK`. Fail path: assertion `BNE fail` branches to `fail: BREAK`.
- **Calling convention:** Return via RET (JMP R13). Program preamble sets LR and calls the test subroutine.
- **Boot from ROM:** Programs are assembled with `--org 0xFFFFE000` and loaded into boot ROM. `_start:` must be the first label in the source file (ROM execution begins at the first word). Programs that use exceptions install handler addresses in the RAM vector table at startup via `LA Rd, #handler` + `STW Rd, [R0 + #offset]`.
- **ROM page mapping:** MMU-enabled tests must map the ROM page in the TLB before enabling the MMU: `TLB_INDEX=30` (set 30, way 0), `TLB_VPN=0x0FFFFE00`, `TLB_PTE=0xFFFFE0B9` (VPN/PPN 0xFFFFE, KERN_RWX).

### Implemented Microcode (41 micro-ops)
| Category | Instructions | Notes |
|----------|-------------|-------|
| ALU (R-ALU, 0x00–0x1E) | ADD, SUB, AND, OR, XOR, SHL, SHR, SAR, MOV, NOT | CMP/TEST via F-bit gating on SUB/AND |
| Immediate (Format L) | LLI, LLIS, LUI, ADD #imm, SUB #imm, CMP #imm | Formerly INC/DEC/CMPI (still accepted as aliases) |
| Memory (Format M) | LDW/LDH/LDHS/LDB/LDBS (3 micro-ops), STW/STH/STB (4 micro-ops) | STALL-based, latency-agnostic; byte_ext extracts on load, byte_rep replicates on store, byte_en selects lanes |
| Branch (Format B) | Bcc (all 15 conditions via single BRT entry), BL (2 micro-ops) | BL saves PC+4 to R13, dispatches to 0x62; BZ/BNZ aliases in assembler |
| System (R-SYS, 0x40–0x5E) | JMP, EI, DI, WRSYS, RDSYS, ERET, WRSPR, RDSPR, SYSCALL, BREAK | RET = JMP R13 (pseudo); ERET/RDSYS are 2-micro-op; SYSCALL/BREAK intercepted at dispatch; WRSPR/RDSPR unified with SPR select in IR[15:12] (ops 27–28, frees ops 29–31) |
| Exception | int_entry (3 micro-ops) | Reads handler address from vector table (MAR←vector_addr, STALL read, PC←MDR). Shared by all exception sources. MMU bypassed for vector read. |

### Known Bugs Fixed (Notable)
- **IR corruption from shared mem_rdata bus:** ir_valid lingered one cycle into S_EXEC, causing IR to reload when mem_rdata was muxed to sysreg data. Fix: `ir_load = ir_valid && fetch_active`.
- **BR_PRIV used advance instead of go_fetch:** (Historical — BR_PRIV removed; privilege now checked via microcode priv bit in sequencer.)
- **reg_w_sel gated by executing:** Could change at same posedge as register write. Ungated to keep address stable.
- **fault_pending cleared too early:** Clearing at `ir_valid` (dispatch) meant `vector_num` was wrong one cycle later when `int_entry` read it. Fix: clear at `ctl_pc_load` (int_entry execution).
- **Vector fetch corrupted fault registers:** MMU bypass for vector fetch only gated the output mux, not the TLB lookup or fault latching. The TLB still reported a miss for the unmapped vector page, overwriting `FAULT_ADDR` with the vector address. Fix: gate `i_lookup_en` and fault latching with `!i_force_bypass`.
- **ERET (was IRET) assembled with wrong opcode:** (Historical — IRET removed; ERET is now the only exception return instruction. WRSPR EPC/ESR replaces the old 2-register ERET for context switches.)

### UART (Memory-Mapped I/O)
The simulation UART (`sim_uart.sv`) is an NS16450-compatible device at `0xFF00_0000`, accessed via LDW/STW (not WRSYS/RDSYS — it's on the memory bus, not the sysreg bus). This matches how real FPGA SoCs work: peripherals are memory-mapped, accessed through the MMU with C=0 (uncached) TLB entries.

**Register map** (word-strided, data in bits [7:0] of each 32-bit word):

| Offset | DLAB=0 R / W | DLAB=1 | Description |
|--------|-------------|--------|-------------|
| 0x000 | RBR / THR | DLL | Receive buffer / Transmit holding / Divisor low |
| 0x004 | IER | DLM | Interrupt enable / Divisor high |
| 0x008 | IIR / FCR | — | Interrupt ID (R) / FIFO control (W, ignored) |
| 0x00C | LCR | — | Line control (DLAB = bit 7) |
| 0x010 | MCR | — | Modem control (OUT2 = bit 3 = master IRQ enable) |
| 0x014 | LSR | — | Line status (bit 0=DR, bit 5=THRE, bit 6=TEMT) |
| 0x018 | MSR | — | Modem status (CTS+DSR hardwired asserted) |
| 0x01C | SCR | — | Scratch register (probe detection) |

- **NetBSD compatible:** Works with NetBSD `com(4)` driver using `reg-shift=2`, `reg-io-width=4`. Platform attachment calls `com_init_regs_stride_width(&regs, bst, bsh, addr, 2, 4)`.
- **TX busy simulation:** After THR write, THRE goes low for `TX_BUSY_CYCLES` (default 2170, matching ~115200 baud at 25 MHz). Ensures polling code exercises the THRE check.
- **Testbench interface:** `o_uart_tx_valid`/`o_uart_tx_data` pulse on TX completion (testbench does `putchar`). `i_uart_rx_valid`/`i_uart_rx_data` + `o_uart_rx_ack` handshake for RX injection.
- **IRQ:** `o_irq` asserted when any enabled interrupt + MCR OUT2. Wired to CPU's `i_irq` (OR'd with external testbench IRQ).
- **Polling pattern:** `LDW LSR, TEST THRE, BZ poll, STW THR` — same as every 16450 driver since 1981.
- **Real hardware:** Replace `sim_uart` with a baud-rate UART (add shift register + baud generator from DLL/DLM). Same register interface. Add 16-byte FIFOs by flipping IIR[7:6] to `11`.

### LLVM Backend (`llvm/llvm/lib/Target/Penumbra/`)
The Penumbra LLVM backend is under development. Target triple: `penumbra-unknown-none` (eventually `penumbra-unknown-netbsd`). Build with `cmake -G Ninja -DLLVM_TARGETS_TO_BUILD=Penumbra` from `llvm/llvm/`, build dir `build/llvm/`. Uses ccache and Ninja. Use `-j2` for link steps (debug builds OOM at full parallelism on 15 GB WSL2).

**Current state:** Target registered (`llc --version` shows `penumbra`), TableGen generates all `.inc` files, libraries compile and link. No MC-layer assembler yet (`llvm-mc` fails with "unable to create instruction printer").

| File | Description |
|------|-------------|
| `Penumbra.td` | Top-level TableGen: includes, ProcessorModel, AsmWriter, Target, pointer remap |
| `PenumbraRegisterInfo.td` | 16 GPRs (R0=zero, R13=LR, R14=SP, R15=PC), GPR/GPR_Allocatable/CCR classes |
| `PenumbraInstrInfo.td` | All 4 instruction formats (R/L/M/B) with bit-accurate encoding. ALU, immediate, memory, branch, system instructions. Format R subclasses for 0-operand and 1-operand system ops |
| `PenumbraTargetMachine.{h,cpp}` | Inherits `CodeGenTargetMachineImpl`, data layout `e-m:e-p:32:32-i32:32-n32-S32` |
| `MCTargetDesc/PenumbraMCAsmInfo.{h,cpp}` | ELF-based, little-endian, `;` comments, `.word`/`.half`/`.byte` directives |
| `MCTargetDesc/PenumbraMCTargetDesc.{h,cpp}` | Registers MC components (InstrInfo, RegInfo, SubtargetInfo, AsmInfo) |
| `TargetInfo/PenumbraTargetInfo.{h,cpp}` | Target registration (`Triple::penumbra`) |

**Next MC-layer pieces needed for `llvm-mc` assembler:**
1. **InstPrinter** — MCInst → assembly text
2. **AsmParser** — assembly text → MCInst
3. **MCCodeEmitter** (C++ wrapper) — MCInst → binary bytes
4. **AsmBackend + ELF object writer** — relaxation, fixups, `.o` emission

**Triple integration:** `penumbra` added to `Triple.h` (arch enum), `Triple.cpp` (name, prefix, parsing, 32-bit, little-endian, no-64-bit-variant, ELF format, DwarfCFI exception handling). Also added to `llvm/llvm/CMakeLists.txt` `LLVM_ALL_TARGETS`. Note: `TargetDataLayout.cpp:computeDataLayout()` has a `-Wswitch` warning for unhandled `penumbra` case — harmless (we provide our own data layout string in PenumbraTargetMachine.cpp).

### Next Steps (in priority order)
1. **LLVM MC-layer assembler** — InstPrinter, AsmParser, MCCodeEmitter, AsmBackend to get `llvm-mc` working.
2. **Boot ROM monitor** — Command parser working (`make simulate`): dump, write, go, help. Next: S-record upload for loading programs over UART.
3. **Timer** — Programmable timer/counter for NetBSD hardclock() scheduler tick.
4. **Interrupt controller** — Multiple devices with priority encoding.
5. **Memory subsystem** — SDRAM controller, bus interface.
