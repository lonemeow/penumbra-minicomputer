# Penumbra Hardware — Claude Code Context

This file provides detailed hardware context for work under `hw/`. The root `CLAUDE.md` has project-wide conventions.

## Implemented RTL Modules (all tested)
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
| PC register | `rtl/core/pc_reg.sv` | 31/31 | PC reg (parameterizable RESET_PC), PC+4 adder, PC+offset adder, EPC |
| MAR | `rtl/core/mar.sv` | 6/6 | Memory address register, loads from R-bus |
| MDR | `rtl/core/mdr.sv` | 7/7 | Memory data register, loads from memory or A-bus |
| Datapath top | `rtl/core/datapath.sv` | 15/15 | Structural wiring of all modules, IR reg, reg addr routing, F-bit gating |
| Microcode ROM | `rtl/core/ucode_rom.sv` | — | 256×51-bit ROM, $readmemh from microcode.hex |
| Sequencer | `rtl/core/sequencer.sv` | — | Micro-PC, branch_cond decode, EI/DI tracking, ei_shadow_clr |
| Byte extractor | `rtl/core/byte_ext.sv` | 19/19 | Sub-word load extraction: byte/half from 32-bit word, sign/zero extend |
| Byte replicator | `rtl/core/byte_rep.sv` | 10/10 | Sub-word store lane positioning: replicate byte/half across all lanes |
| CPU core | `rtl/core/cpu_core.sv` | 30 progs | Full CPU: datapath + sequencer + ROM + MMU + split I/D cache + memory bus mux + fetch + IRQ + MMU traps (data + fetch) + alignment faults + bus faults + BREAK + SYSCALL + privilege traps + illegal instruction trap + WRSYS/RDSYS + RDSPR/WRSPR + BL + sub-word loads/stores. Parameterizable RESET_PC (default 0xFFFF_E000). |
| Sim machine | `rtl/soc/machine_sim.sv` | (top) | Simulation integration: cpu_core + boot_rom + simple_mem + sim_uart + sysid. Shared-bus model with device-side address decode via `bus_devsel`. UART IRQ wired to CPU. |
| Bus devsel | `rtl/soc/bus_devsel.sv` | via machine_sim | Combinational address comparator for device-side bus decode. Parameterized BASE/SIZE. |
| Sim UART | `rtl/soc/sim_uart.sv` | via machine_sim | 16450-compatible UART (MMIO at 0xFF00_0000). NetBSD com(4) compatible via reg-shift=2, reg-io-width=4 |
| Boot ROM | `rtl/soc/boot_rom.sv` | via machine_sim | Read-only memory (8 KB default), loads program.hex |
| Shared package | `rtl/core/penumbra_pkg.sv` | — | REG_*, ALU_*, COND_*, SR_*, ACC_*, VEC_*, FAULT_*, FSTAT_*, SYSDEV_*, SYSREG_*, CACHE_TYPE_*, UART_*, SPR_*, RAM_BASE, ROM_BASE constants |
| System ID | `rtl/soc/sysid.sv` | via machine_sim | Read-only MACHINE_ID register (Penumbra/1), sysreg device 1 |
| TLB | `rtl/mmu/tlb.sv` | 111/111 | 64-entry 2-way SA, parallel lookup, one-hot permission check, indexed sysreg R/W |
| MMU | `rtl/mmu/mmu.sv` | — | Bypass/translate mux, force_bypass for vector table read, alignment check, sysreg routing, fault latching, TLB instantiation |
| Cache | `rtl/soc/cache.sv` | 42/42 | Parameterized PIPT cache (NUM_SETS, LINE_WORDS, NUM_WAYS). Write-through/write-no-allocate. Burst line fill on read miss. Sysreg interface. |
| Cache stub | `rtl/soc/cache_stub.sv` | — | Combinational pass-through, retained for reference. Replaced by cache.sv in cpu_core. |
| Simple memory | `rtl/soc/simple_mem.sv` | — | Parameterizable synchronous SRAM model (default 16 MB), configurable READ_LATENCY/WRITE_LATENCY modeling SDRAM timing. |

## Boot ROM and Interactive Simulation
- **Boot ROM** (`rom/boot_rom.c` + `rom/crt0.s`): Penumbra/1 boot monitor in C. `crt0.s` sets SP and calls `main()`. Built with clang: `clang -c` → `llvm-mc` → `ld.lld` (via `rom/rom.ld`) → `llvm-objcopy` → `bin2hex.py`.
- **Interactive testbench** (`sim/tb_interactive.cpp`): Bridges host stdin/stdout to UART RX/TX. Raw terminal mode. Polls stdin every 1024 cycles. Exits on BREAK or Ctrl-C.

## Exception and Interrupt Handling
Eight sources share the same `except_entry` → `int_entry` → vector dispatch path:

**External IRQ (asynchronous):**
- Check at dispatch-time (`ir_valid`). `irq_taken = i_irq & sr_i & !ei_shadow`.
- Override dispatch to 0x70 (int_entry), pulse `except_entry` (saves EPC/ESR, sets S=1/I=0).
- EI sets sr_i=1 and ei_shadow=1 (cleared after next instruction). DI sets sr_i=0 immediately (privileged).

**MMU data fault (synchronous):**
- Check during STALL on load/store. `data_fault = mmu_fault && !fetch_active`.
- `fault_except` pulse → `except_entry`. Sequencer aborts STALL, returns to S_FETCH.
- `fault_pending` overrides next dispatch to int_entry with fault vector. Cleared at `ctl_pc_load`.
- Priority: fault > illegal > priv > BREAK > SYSCALL > IRQ.

**MMU instruction fetch fault (synchronous):**
- Check during S_FETCH. `fetch_fault = mmu_fault && fetch_active`.
- I-cache gated: `i_re = fetch_active && !mmu_fault`. IR load gated by `!fault_pending`.
- `break_taken`/`syscall_taken` gated by `!fault_pending` to prevent stale `mem_rdata` dispatch.

**Alignment fault (synchronous, fetch or data):**
- MMU checks alignment via `i_mem_size`. Fires even in bypass mode. Checked before TLB lookup.
- Vector VEC_ALIGN=8. FAULT_STATUS includes access type (code/data).

**Bus fault (synchronous, fetch or data):**
- Fires when physical bus request hits no device. Wired from `machine_sim` to `cpu_core.i_bus_fault`.
- Vector VEC_BUS_FAULT=0. Highest priority in `fault_vector` selection.
- Use cases: RAM probing at boot, device probing with MMU enabled (NetBSD `bus_space_peek`).

**BREAK instruction:** Dispatch-time (`0x4A`), vectors to VEC_BREAK (6). `o_halted` pulses for testbench.

**SYSCALL instruction:** Dispatch-time (`0x48`), vectors to VEC_SYSCALL (5). EPC points at SYSCALL; handler must advance EPC+4.

**Privilege violation:** First micro-op of S_EXEC checks `priv=1 && !sr_s`. Suppresses all enables, vectors to VEC_PRIV (4).

**Illegal instruction:** First micro-op detects sentinel (`branch==BR_ILLEGAL`). Vectors to VEC_ILLEGAL (7).

**Vector table (MIPS/68k-style):** Physical 0x00, contains handler addresses (not instructions). `int_entry` reads handler via MDR, bypasses MMU. Vectors: BUS_FAULT=0, IRQ=1, TLB_MISS=2, TLB_PROT=3, PRIV=4, SYSCALL=5, BREAK=6, ILLEGAL=7, ALIGN=8.

**Reset:** CPU boots at `RESET_PC` (default `0xFFFF_E000`), hardwired — not part of vector table.

**Dispatch-time vector latching:** `dispatch_pending`/`dispatch_vector` register vector number at `ir_valid` because combinational inputs change between dispatch and int_entry execution.

## Register Address Routing
The micro-word's `reg_a_sel`, `reg_b_sel`, `reg_w_sel` fields use a 4-bit encoding:
- `4'b0000` (IR_RD): format-dependent destination register (R→IR[24:21], L→IR[25:22], M→IR[25:22])
- `4'b0001` (IR_RS): format-dependent source/base register (R→IR[20:17], M→IR[21:18])
- `4'b0010–4'b1111`: literal register R2–R15

F-bit write-enable gating only applies when `reg_w_sel = IR_RD` (not for literal addresses).

## Memory Access
- **STALL-based:** Load/store micro-routines use `branch=STALL`. Same microcode works regardless of memory latency.
- **Split I/D caches** between CPU and memory. Cache hits: zero latency; misses: burst-fill. Memory bus mux merges both caches (D-cache priority; fetch and data mutually exclusive).
- **MMU traps:** STALL path checks `i_mem_fault` alongside `i_mem_busy`. On fault, sequencer aborts to S_FETCH.
- **Dispatch spacing:** Format R: ×2 split by op[4] (ALU 0x00–0x1E, SYS 0x40–0x5E). Format M: ×4 (0x80–0xBF).

## Implemented Microcode (43 micro-ops)
| Category | Instructions | Notes |
|----------|-------------|-------|
| ALU (R-ALU, 0x00–0x1E) | ADD, SUB, AND, OR, XOR, SHL, SHR, SAR, MOV, NOT | CMP/TEST via F-bit gating on SUB/AND |
| Immediate (Format L) | LLI, LLIS, LUI, ADD #imm, SUB #imm, CMP #imm, AND #imm, TEST #imm, SHL #imm, SHR #imm, SAR #imm | 11 of 16 Format L slots used |
| Memory (Format M) | LDW/LDH/LDHS/LDB/LDBS (3 µ-ops), STW/STH/STB (4 µ-ops) | STALL-based, latency-agnostic |
| Branch (Format B) | Bcc (all 15 conditions), BL (2 µ-ops) | BL saves PC+4 to R13 |
| System (R-SYS, 0x40–0x5E) | JMP, EI, DI, WRSYS, RDSYS, ERET, WRSPR, RDSPR, SYSCALL, BREAK | SYSCALL/BREAK intercepted at dispatch |
| Exception | int_entry (3 µ-ops) | Reads handler from vector table, MMU bypassed |

## UART (Memory-Mapped I/O)
NS16450-compatible at `0xFF00_0000`, accessed via LDW/STW (memory bus, not sysreg bus).

**Register map** (word-strided, data in bits [7:0]):

| Offset | DLAB=0 R / W | DLAB=1 | Description |
|--------|-------------|--------|-------------|
| 0x000 | RBR / THR | DLL | Receive buffer / Transmit holding / Divisor low |
| 0x004 | IER | DLM | Interrupt enable / Divisor high |
| 0x008 | IIR / FCR | — | Interrupt ID / FIFO control |
| 0x00C | LCR | — | Line control (DLAB = bit 7) |
| 0x010 | MCR | — | Modem control (OUT2 = bit 3 = master IRQ enable) |
| 0x014 | LSR | — | Line status (bit 0=DR, bit 5=THRE, bit 6=TEMT) |
| 0x018 | MSR | — | Modem status (CTS+DSR hardwired asserted) |
| 0x01C | SCR | — | Scratch register |

- NetBSD `com(4)` compatible: `reg-shift=2`, `reg-io-width=4`.
- TX busy simulation: THRE low for `TX_BUSY_CYCLES` (default 2170, ~115200 baud at 25 MHz).
- Testbench: `o_uart_tx_valid`/`o_uart_tx_data` for TX, `i_uart_rx_valid`/`i_uart_rx_data` + `o_uart_rx_ack` for RX.
- IRQ: `o_irq` when enabled interrupt + MCR OUT2.
- Polling: `LDW LSR, TEST THRE, BZ poll, STW THR`.
