# Penumbra Minicomputer - Claude Code Context

## Project Overview
Penumbra is a 32-bit RISC-like minicomputer designed from scratch and implemented on a Radiona ULX3S (Lattice ECP5) FPGA. The project covers the full system: CPU, MMU, DMA, I/O, and system bus. The eventual goal is to port NetBSD, and later build the design from discrete 74xx chips.

## Key Decisions
- **HDL:** SystemVerilog for RTL
- **Toolchain:** Open-source FPGA tools (Yosys, nextpnr-ecp5, Project Trellis)
- **Simulation:** Verilator 5.046 via Docker (`verilator/verilator:latest`), driven by Makefile
- **Target board:** ULX3S with ECP5-85F (32 MB SDRAM, USB, HDMI, GPIO, etc.)
- **OS target:** NetBSD (drives privilege, interrupt, MMU design). See `doc/toolchain/toolchain-strategy.md`
- **Compiler:** LLVM backend (GlobalISel, not SelectionDAG). ABI spec in `doc/abi/penumbra-abi.md`
- **Discrete build:** All design decisions must be feasible in 74xx discrete logic
- **Byte order:** Little-endian. `addr[1:0]=00` maps to bits `[7:0]` (defined by `byte_ext`/`byte_rep`). Matches x86/RISC-V/ARM-LE.
- **MUL/DIV/FP strategy:** Unified ALU, multi-cycle ops use alu_start/alu_busy. MUL/DIV initially trapped as illegal, SW emulated, hardware added incrementally.

## Architecture Summary
The architecture is fully specified in `doc/`. Key specs:
- **ISA:** `doc/isa/architecture-overview.md` — 4-format 32-bit encoding (R/L/M/B), 2-operand, R0=zero, 16 registers, ARM-style condition flags
- **Datapath:** `doc/core/datapath.md` — three-bus (A/B/R), separate PC unit, 51-bit horizontal microcode, hardwired fetch unit, direct-mapped dispatch
- **Microcode reference:** `doc/core/microcode-reference.md` — complete micro-word format, field reference, ROM layout, sequencer behavior
- **Bus:** `doc/bus/bus-overview.md` — custom async Penumbra Bus (4-phase handshake), sync internal bus, sysreg sideband
- **MMU/Cache:** `doc/mmu/mmu-overview.md` — software-managed 64-entry 2-way SA TLB, split I/D PIPT cache, write-through D-cache
- **Sysregs:** `doc/isa/sysregs-reference.md` — WRSYS/RDSYS device map, register layouts, TLB packing
- **ABI:** `doc/abi/penumbra-abi.md` — ILP32, R1–R4 args, R5–R10 callee-saved, R11 scratch, R12 TP, R13 LR, R14 SP, R15 PC

## Repository Layout
- `hw/` - All hardware design (RTL, testbenches, microcode, boot ROM, tools). See `hw/CLAUDE.md` for detailed hardware context.
- `sw/` - Software tools (`sw/tools/pasm.py` assembler, `sw/tools/bin2hex.py` converter)
- `llvm/` - LLVM backend. See `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` for detailed LLVM context.
- `doc/` - Architecture specs (ISA, MMU, bus, memory map, datapath, toolchain, ABI)

## Conventions
- RTL filenames match the top-level module they contain
- One module per file
- Use `logic` rather than `reg`/`wire` where possible
- Prefix module ports: `i_` for inputs, `o_` for outputs
- Clock signal: `i_clk`, synchronous active-high reset: `i_rst` — sampled on rising edge of `i_clk`; while asserted, all state holds reset values; testbench holds for 2 cycles then releases
- Shared constants in `hw/rtl/core/penumbra_pkg.sv` (register addresses, ALU opcodes, condition codes)
- Modules that use the package: `import penumbra_pkg::*;` inside the module declaration (not at file scope — Verilator warns about `import *` at $unit scope)

### Naming: Hardware vs Software Terminology
- **Supervisor** = hardware privilege level (SR.S bit, CPU mode). Use for CPU-level concepts.
- **Kernel** = OS software running in supervisor mode. Use for OS-level concepts.
- **Special-purpose registers (SPRs)** = CPU-internal registers via `RDSPR`/`WRSPR` (EPC, ESR, USP). SPR number encoded in IR[15:12].
- **System registers (sysregs)** = device-mapped registers via `WRSYS`/`RDSYS` (MMU, TLB, system ID). Belong to peripheral devices.

## Build System
- `make smoke` — toolchain smoke test (trivial adder)
- `make sim MOD=<name>` — build & run a module's Verilator testbench
- `make sim MOD=machine_sim TB=<tb> PROG=<prog>` — run specific testbench with specific program. Auto-assembles `.s`/`.uasm` into hex.
- `make test` — run all `hw/sim/programs/test_*.s` programs; reports pass/fail summary
- `make simulate` — build C boot ROM via clang pipeline, run interactive simulator with terminal I/O via Docker (`-it`). Override LLVM location: `make simulate LLVM_PREFIX=/path/to/llvm-build`
- `make wave MOD=<name>` — open VCD waveform in GTKWave
- `make clean` — remove build artifacts
- All simulation runs via Docker — no host install needed. Build artifacts in `build/` (gitignored).
- **Important:** `rm -rf build/<mod>.verilator build/V<mod>` if you suspect stale binaries (WSL2 stale mtimes)

### Boot ROM Build Pipeline (`make simulate`)
The boot ROM has its own Makefile (`hw/rom/Makefile`) with automatic source discovery (all `*.c` files), pattern rules, and header dependency tracking via `-MMD -MP`. The main Makefile delegates with `$(MAKE) -C hw/rom`. Can also be built standalone: `make -C hw/rom`.
```
hw/rom/*.c    →  clang -c  →  *.o  ─┐
hw/rom/crt0.s →  llvm-mc   →  crt0.o ├→ ld.lld (rom.ld) → boot_rom.elf → objcopy → bin2hex → program.hex
hw/rom/rom.ld ─────────────────────────┘
```

## Current Status
The CPU is fully functional in simulation: all RTL modules implemented and tested, CPU runs real programs through the full CPU → MMU → split I/D cache → memory path, booting from ROM at `0xFFFF_E000`. Eight exception sources (IRQ, MMU faults, alignment, bus fault, BREAK, SYSCALL, privilege, illegal) are fully wired with MIPS/68k-style vector dispatch.

**LLVM toolchain is end-to-end functional.** The boot ROM is compiled from C using clang, linked with lld, and runs on the simulated CPU. Full pipeline: `clang -c` → `ld.lld` → `llvm-objcopy` → `bin2hex.py` → simulator. MC-layer assembler produces working ELF objects with 7 relocation types (NONE, 32, BRANCH22, IMM16, MEMOFFSET16, LO16, HI16). Assembly pseudo-instructions LI, LA, NOP, RET expand in the AsmParser. Register aliases (pc, sp, lr, zero, tp), SPR names (epc, esr, usp), memory shorthand (`[Rb]`), and expression offsets supported. RDSPR/WRSPR/RDSYS/WRSYS fully encoded. GlobalISel codegen handles i32 ALU, constants, global addresses (LLI+LUI with lo16/hi16), sub-word load/store (LDB/LDH/LDW, STB/STH/STW), pointer arithmetic, G_FRAME_INDEX (memory folding + LEAfi), jump tables (G_JUMP_TABLE + G_BRJT → BRIND), calling convention (R13/LR callee-saved), control flow, G_SELECT, extensions/truncation, INTTOPTR/PTRTOINT, MUL/DIV/REM via libcalls, pointer comparisons. `-O0` works; `-O1+` needs more legalization rules (G_SMAX, etc.).

## Software Tools
- **LLVM toolchain** (`build/llvm/bin/`, override with `LLVM_PREFIX`): clang (C compiler), llvm-mc (assembler), ld.lld (linker), llvm-objcopy. Target triple: `penumbra-unknown-none`. See `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` for backend details.
- **Microcode assembler** (`hw/tools/uasm.py`): Symbolic microcode → $readmemh hex. Run: `python3 hw/tools/uasm.py input.uasm -o microcode.hex`
- **ISA assembler** (`sw/tools/pasm.py`): Two-pass assembler, all 4 formats, labels, pseudo-ops (NOP, RET, LA, LI), `.equ`, data directives. Still used by `make test` for hardware test programs. Run: `python3 sw/tools/pasm.py --org 0xFFFFE000 input.s -o program.hex`
- **Binary-to-hex converter** (`sw/tools/bin2hex.py`): Flat LE binary → $readmemh hex. Used in the clang pipeline.
- Hex files are gitignored. Makefile auto-builds them from sources.

## Test Convention
- **Program runner** (`hw/sim/tb_cpu_prog.cpp`): Runs program until BREAK (500000 cycle limit), checks R1 for pass/fail.
- **Pass/fail:** R1 = 1 means PASS, R1 = 0 means FAIL. Tests self-check and set R1.
- **Halt:** Testbench watches `o_halted` pulse (BREAK dispatch). Programs end with `BREAK`.
- **Boot from ROM:** Programs assembled with `--org 0xFFFFE000`. `_start:` must be first label.
- **ROM page mapping (MMU tests):** TLB_INDEX=30, TLB_VPN=0x0FFFFE00, TLB_PTE=0xFFFFE0B9.

## Next Steps (in priority order)
1. **LLVM codegen hardening** — `-O1+` support (G_SMAX/G_SMIN legalization, MUL/DIV libcalls or traps), `%lo16()`/`%hi16()` AsmParser parsing (assembly text roundtrip), register aliases (sp, lr, pc, zero, tp), WRSYS/RDSYS encoding
2. **Boot ROM monitor** — Rewrite UART I/O and command loop in C (d/w/g commands), S-record upload for loading programs over UART
3. **Timer** — Programmable timer/counter for NetBSD hardclock() scheduler tick
4. **Interrupt controller** — Multiple devices with priority encoding
5. **Memory subsystem** — SDRAM controller, bus interface
