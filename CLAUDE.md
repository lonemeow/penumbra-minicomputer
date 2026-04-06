# Penumbra Minicomputer - Claude Code Context

## Project Overview
Penumbra is a 32-bit RISC-like minicomputer designed from scratch
and implemented on a Radiona ULX3S (Lattice ECP5) FPGA.
The project covers the full system: CPU, MMU, DMA, I/O, and system bus.
The eventual goal is to port NetBSD,
and later build the design from discrete 74xx chips.

## Key Decisions
- **HDL:** SystemVerilog for RTL
- **Toolchain:** Open-source FPGA tools (Yosys, nextpnr-ecp5, Project Trellis)
- **Simulation:** Verilator 5.046 via Docker (`verilator/verilator:latest`), driven by Makefile
- **Target board:** ULX3S with ECP5-85F (32 MB SDRAM, USB, HDMI, GPIO, etc.)
- **OS target:** NetBSD (drives privilege, interrupt, MMU design). See `doc/toolchain/toolchain-strategy.md`
- **Compiler:** LLVM backend (GlobalISel, not SelectionDAG). ABI spec in `doc/abi/penumbra-abi.md`
- **Discrete build:** All design decisions must be feasible in 74xx discrete logic
- **Byte order:** Little-endian. `addr[1:0]=00` maps to bits `[7:0]`
  (defined by `byte_ext`/`byte_rep`). Matches x86/RISC-V/ARM-LE.
- **MUL/DIV/FP strategy:** Unified ALU, multi-cycle ops use alu_start/alu_busy.
  MUL/DIV initially trapped as illegal, SW emulated, hardware added incrementally.

## Architecture Summary
The architecture is fully specified in `doc/`. Key specs:
- **ISA:** `doc/isa/architecture-overview.md` —
  4-format 32-bit encoding (R/L/M/B), 2-operand, R0=zero,
  16 registers, ARM-style condition flags
- **Datapath:** `doc/core/datapath.md` —
  three-bus (A/B/R), separate PC unit, 51-bit horizontal microcode,
  hardwired fetch unit, direct-mapped dispatch
- **Microcode reference:** `doc/core/microcode-reference.md` —
  complete micro-word format, field reference, ROM layout, sequencer behavior
- **Bus:** `doc/bus/bus-overview.md` —
  custom async Penumbra Bus (4-phase handshake), sync internal bus,
  sysreg sideband
- **MMU/Cache:** `doc/mmu/mmu-overview.md` —
  software-managed 64-entry 2-way SA TLB, split I/D PIPT cache,
  write-through D-cache
- **Sysregs:** `doc/isa/sysregs-reference.md` —
  WRSYS/RDSYS device map, register layouts, TLB packing
- **ABI:** `doc/abi/penumbra-abi.md` —
  ILP32, R1–R4 args, R5–R10 callee-saved, R11 scratch,
  R12 TP, R13 LR, R14 SP, R15 PC

## Repository Layout
- `hw/` - All hardware design (RTL, testbenches, microcode, boot ROM, tools).
  See `hw/CLAUDE.md` for detailed hardware context.
- `sw/` - Software tools (`sw/tools/pasm.py` assembler, `sw/tools/bin2hex.py` converter)
- `llvm/` - LLVM backend. See `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` for detailed LLVM context.
- `doc/` - Architecture specs (ISA, MMU, bus, memory map, datapath, toolchain, ABI)

## Conventions
- RTL filenames match the top-level module they contain
- One module per file
- Use `logic` rather than `reg`/`wire` where possible
- Prefix module ports: `i_` for inputs, `o_` for outputs
- Clock signal: `i_clk`, synchronous active-high reset: `i_rst` —
  sampled on rising edge of `i_clk`; while asserted, all state holds
  reset values; testbench holds for 2 cycles then releases
- Shared constants in `hw/rtl/core/penumbra_pkg.sv`
  (register addresses, ALU opcodes, condition codes)
- Modules that use the package: `import penumbra_pkg::*;` inside the
  module declaration (not at file scope —
  Verilator warns about `import *` at $unit scope)

### Naming: Hardware vs Software Terminology
- **Supervisor** = hardware privilege level (SR.S bit, CPU mode). Use for CPU-level concepts.
- **Kernel** = OS software running in supervisor mode. Use for OS-level concepts.
- **Special-purpose registers (SPRs)** = CPU-internal registers via
  `RDSPR`/`WRSPR` (ESR, EPC, USP, SR). SPR number encoded in IR[15:12].
- **System registers (sysregs)** = device-mapped registers via
  `WRSYS`/`RDSYS` (MMU, TLB, system ID). Belong to peripheral devices.

## Build System
- `make smoke` — toolchain smoke test (trivial adder)
- `make sim MOD=<name>` — build & run a module's Verilator testbench
- `make sim MOD=machine_sim TB=<tb> PROG=<prog>` —
  run specific testbench with specific program.
  Auto-assembles `.s`/`.uasm` into hex.
- `make test` — run all `hw/sim/programs/test_*.s` programs;
  reports pass/fail summary
- `make simulate` — build C boot ROM via clang pipeline,
  run interactive simulator with terminal I/O via Docker (`-it`).
  - Override LLVM location: `make simulate LLVM_PREFIX=/path/to/llvm-build`
  - Non-interactive (piped input): `echo "break" | make simulate INTERACTIVE=0`
  - With SD card image: `make simulate SDCARD=disk.img`
  - Instruction trace: `make simulate TRACE=build/trace.log`
    (dumps PC, SR, R1–R14 for every instruction)
  - ROM monitor accepts `break` (or `b`) to halt the simulator cleanly.
- `make wave MOD=<name>` — open VCD waveform in GTKWave
- `make clean` — remove build artifacts
- All simulation runs via Docker — no host install needed. Build artifacts in `build/` (gitignored).
- **Important:** `rm -rf build/<mod>.verilator build/V<mod>`
  if you suspect stale binaries (WSL2 stale mtimes)

### LLVM Toolchain Build
Build dir: `build/llvm/`. Initial cmake (one-time):
```sh
cmake -G Ninja -S llvm/llvm -B build/llvm \
  -DLLVM_TARGETS_TO_BUILD=Penumbra \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_USE_SPLIT_DWARF=ON \
  -DLLVM_INCLUDE_TESTS=ON -DLLVM_BUILD_TESTS=ON \
  -DLLVM_PARALLEL_LINK_JOBS=2
```
Incremental rebuild — **target only what's needed** to avoid
building all unit tests:
```sh
ninja -C build/llvm -j10 llc clang lld
```

### LLVM Backend Tests
Regression tests: `llvm/llvm/test/CodeGen/Penumbra/` (Lit + FileCheck).
```sh
build/llvm/bin/llvm-lit llvm/llvm/test/CodeGen/Penumbra/       # all
build/llvm/bin/llvm-lit -v llvm/llvm/test/CodeGen/Penumbra/alu.ll  # one
```
Regenerate CHECK lines after codegen changes:
```sh
python3 llvm/llvm/utils/update_llc_test_checks.py \
  --llc-binary build/llvm/bin/llc \
  llvm/llvm/test/CodeGen/Penumbra/<test>.ll
```

### NetBSD Kernel Build
Prerequisites: NetBSD tools built via `build.sh` (one-time, see
`netbsd/sys/arch/penumbra/CLAUDE.md`).
Build output: `build/netbsd-kernel/MINIMAL/` (out of source tree).
All commands from project root:
```sh
# 1. Generate kernel Makefile (re-run after conf/ changes)
build/netbsd-tools/bin/nbconfig \
  -b $PWD/build/netbsd-kernel/MINIMAL \
  -s $PWD/netbsd/sys \
  $PWD/netbsd/sys/arch/penumbra/conf/MINIMAL

# 2. Dependencies + build
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL depend
build/netbsd-tools/bin/nbmake-penumbra -C build/netbsd-kernel/MINIMAL -j10
```

### Boot ROM Build Pipeline (`make simulate`)
The boot ROM has its own Makefile (`hw/rom/Makefile`) with automatic
source discovery (all `*.c` files), pattern rules, and header
dependency tracking via `-MMD -MP`.
The main Makefile delegates with `$(MAKE) -C hw/rom`.
Can also be built standalone: `make -C hw/rom`.
```
hw/rom/*.c    →  clang -c  →  *.o  ─┐
hw/rom/crt0.s →  llvm-mc   →  crt0.o ├→ ld.lld (rom.ld) → boot_rom.elf → objcopy → bin2hex → program.hex
hw/rom/rom.ld ─────────────────────────┘
```

## Current Status
The CPU is fully functional in simulation: all RTL modules implemented
and tested, CPU runs real programs through the full
CPU → MMU → split I/D cache → memory path,
booting from ROM at `0xFFFF_0000`.
Eight exception sources (IRQ, MMU faults, alignment, bus fault,
BREAK, SYSCALL, privilege, illegal) are fully wired with
MIPS/68k-style vector dispatch.

**Bus autoconfig and SD card boot path working end-to-end.**
- Bus controller sysreg (`busctl.sv`, device 4),
  autoconfig wrapper (`autoconfig_dev.sv`),
  and SPI controller (`sim_spi.sv`) are implemented.
- Boot ROM runs autoconfig: resets bus, enables config chain,
  probes devices via bus-fault detection,
  allocates base addresses, and configures devices.
  Protocol requires software to toggle CFG_EN between devices
  (see `doc/bus/bus-overview.md`).
- SPI controller is the first autoconfigured device;
  reports as `CLASS_SD`, assigned `0xFF001000`.
- Testbench SD card emulator (`sd_card_sim.h`) speaks SD-SPI protocol
  backed by a disk image file (`+sdcard=`).
- Boot ROM builds a tagged list of boot data at 0x0040
  (after trap vectors): RAM regions,
  device list (UART injected + autoconfig), console, boot device.
- SD cards detected at boot and accessed via monitor commands.
  MBR partition table parsing implemented:
  `part sd:<dev>,<cs>` displays partitions,
  `load sd:<dev>,<cs>[:<part>] <addr> <lba> <count>` reads sectors
  (raw absolute LBA without `:<part>`,
  or partition-relative LBA with it).
- **ROM FAT32 boot:** `boot sd:<dev>,<cs>` mounts the first
  FAT32 partition, loads `PENBOOT.ELF` (PIE ELF) from root
  directory. The ROM parses ELF headers, allocates RAM for
  scratch and load destination via `find_memory_region()`
  (walks boot data MEMORY devices, avoids reserved areas),
  copies PT_LOAD segments, and jumps to the entry point
  with R1=bootdata. No hardcoded load address.
  FAT32 reader (`fat32.c`) uses a block-read callback for
  device independence.
  Tested end-to-end with `nbmakefs`-generated images.
- SD naming uses per-class controller index
  (`sd:0,0` = first SD controller, CS0),
  not the global device index.
- All SD state is stateless (init → read → deinit per operation)
  to support card hot-swap.

**LLVM toolchain is end-to-end functional.**
- Boot ROM compiled from C using clang, linked with lld,
  runs on the simulated CPU.
- Full pipeline: `clang -c` → `ld.lld` → `llvm-objcopy`
  → `bin2hex.py` → simulator.
- MC-layer assembler produces working ELF objects with
  10 relocation types (NONE, 32, BRANCH22, IMM16,
  LO16, HI16, MEMOFFSET16_PCREL, IMM16_PCREL, RELATIVE).
  Relocation names registered in `ELFRelocs/Penumbra.def`
  for `llvm-readobj`.
- Disassembler fully functional: `llvm-objdump -d` and
  `llvm-mc -disassemble` decode all instruction formats.
  Auto-generated decoder tables with custom decoders for
  branch targets, signed immediates, and memory offsets.
  `EM_PENUMBRA` mapped in `ELFObjectFile.h` for all binary utilities.
- Assembly pseudo-instructions LI, LA, NOP, RET expand
  in the AsmParser.
  Register aliases (pc, sp, lr, zero, tp),
  SPR names (epc, esr, usp, sr),
  memory shorthand (`[Rb]`), and expression offsets supported.
  RDSPR/WRSPR/RDSYS/WRSYS fully encoded.
- GlobalISel codegen uses a hybrid TableGen + C++ approach:
  simple patterns (ALU reg-reg/reg-imm, shifts, NOT, constants,
  all load/store variants) are TableGen `Pat<>` rules in
  `PenumbraGISel.td`; complex multi-instruction sequences
  (wide constants, global addresses, frame-index folding,
  ICMP+BRCOND fold, jump tables, extensions) remain in manual C++.
- **Codegen covers:** i32 ALU with immediate folding
  (ADDi/SUBi/ANDi/SHLi/SHRi/SARi),
  constants (LLI/LLIS/LUI),
  global addresses (LLI+LUI with lo16/hi16),
  sub-word load/store (LDB/LDH/LDW, STB/STH/STW
  with GPRz zero-register substitution),
  pointer arithmetic,
  G_FRAME_INDEX (memory folding + LEAfi),
  jump tables (G_JUMP_TABLE + G_BRJT → BRIND),
  calling convention (R13/LR callee-saved, i64 return in R1:R2),
  i64 support (arg/return splitting, bitwise ops, multi-word compare;
  i64 add/sub via G_UADDO/G_UADDE,
  zext/sext i32→i64 via narrowScalarIf),
  control flow, G_SELECT, extensions/truncation,
  INTTOPTR/PTRTOINT,
  MUL/DIV/REM via libcalls
  (s32 with strength reduction: constant power-of-2 MUL→SHL,
  power-of-2±1 MUL→SHL+ADD/SUB,
  power-of-2 UDIV→SHR, power-of-2 UREM→AND;
  s64 via libcalls),
  G_MEMCPY/G_MEMMOVE/G_MEMSET via libcalls,
  G_CTTZ/G_CTLZ/G_CTPOP (lowered to shift/logic),
  pointer comparisons, G_FREEZE, G_ABS,
  varargs (G_VASTART/G_VAARG with R1-R4 register save area),
  inline assembly (`r`/`i` constraints,
  `~{cc}`/`~{memory}` clobbers),
  G_IMPLICIT_DEF.
- `-fPIC` supported: PC-relative addressing via
  MOV PC + ADDi `%pcrel()` for globals,
  EK_LabelDifference32 jump table entries;
  PC (R15) is a readable GPR so PIC needs no GOT (±32KB reach).
- `-O0` works fully (kernel compiles all .o files at `-O0`).
  `-O1`/`-O2` work for most code but need
  `analyzeBranch`/`insertBranch`/`removeBranch` in TargetInstrInfo
  for branch optimization passes; kernel builds at `-O0` for now.

## Software Tools
- **LLVM toolchain** (`build/llvm/bin/`, override with `LLVM_PREFIX`):
  clang (C compiler), llvm-mc (assembler), ld.lld (linker),
  llvm-objcopy. Target triple: `penumbra-unknown-none`.
  See `llvm/llvm/lib/Target/Penumbra/CLAUDE.md` for backend details.
- **Microcode assembler** (`hw/tools/uasm.py`):
  Symbolic microcode → $readmemh hex.
  Run: `python3 hw/tools/uasm.py input.uasm -o microcode.hex`
- **ISA assembler** (`sw/tools/pasm.py`):
  Two-pass assembler, all 4 formats, labels,
  pseudo-ops (NOP, RET, LA, LI), `.equ`, data directives.
  Still used by `make test` for hardware test programs.
  Run: `python3 sw/tools/pasm.py --org 0xFFFF0000 input.s -o program.hex`
- **Binary-to-hex converter** (`sw/tools/bin2hex.py`):
  Flat LE binary → $readmemh hex. Used in the clang pipeline.
- Hex files are gitignored. Makefile auto-builds them from sources.

## Test Convention
- **Program runner** (`hw/sim/tb_cpu_prog.cpp`):
  Runs program until BREAK (500000 cycle limit),
  checks R1 for pass/fail.
- **Pass/fail:** R1 = 1 means PASS, R1 = 0 means FAIL.
  Tests self-check and set R1.
- **Halt:** Testbench watches `o_halted` pulse (BREAK dispatch). Programs end with `BREAK`.
- **Boot from ROM:** Programs assembled with `--org 0xFFFF0000`. `_start:` must be first label.
- **ROM page mapping (MMU tests):** TLB_INDEX=16, TLB_VPN=0x0FFFF000, TLB_PTE=0xFFFF00B9.

**NetBSD kernel links successfully.**
- Machine headers (39 files), kernel config, MD build system,
  stub kernel sources, and assembly string functions all present.
- `config MINIMAL` → `make depend` → `make` produces a 4 MB
  ELF kernel binary at `build/netbsd-kernel/MINIMAL/netbsd`.
- All MD functions are either implemented or break-trap stubs
  (grep for `TODO(stub)` to find stubs needing real implementations).
- Atomics: interrupt-disable CAS (`RDSPR SR` / `DI` / load-cmp-store
  / `WRSPR SR`), generic CAS-based inc/dec/add/and/or, no-op membars.
  TODO: replace with RAS (Restartable Atomic Sequences) once kernel
  RAS infrastructure is in place.
- DDB (kernel debugger) disabled for now — needs extensive MD hooks.
- Virtual memory layout: 2G/2G user/kernel split,
  kernel text at `0x8001_0000`, compact user layout with
  stack at 64 MB for flat single-level page table optimization.
- Kernel build output in `build/netbsd-kernel/MINIMAL/`
  (out of source tree).
- See `netbsd/sys/arch/penumbra/CLAUDE.md` for detailed
  kernel port context.

## Next Steps (in priority order)
1. **Kernel locore.S** — PIC entry stub in locore.S: compute
   virt-to-phys offset from bootinfo, set up initial TLB entries
   for kernel text + stack + UART, enable MMU, jump to virtual
   entry.  Bootloader already loads kernel and jumps with MMU off.
   See `doc/boot/boot-process.md`
2. **Kernel implementation** — fill in MD stubs (grep `TODO(stub)`):
   copy bootinfo to BSS, parse memory, trap handling,
   console driver, pmap (software TLB);
   get to `main()` → `cpu_startup()`
3. **LLVM `-O2` support** — implement `analyzeBranch`/`insertBranch`/
   `removeBranch` for branch optimization passes
4. **Timer** — Programmable timer/counter for NetBSD hardclock() scheduler tick
5. **Interrupt controller** — Multiple devices with priority encoding
6. **Memory subsystem** — SDRAM controller, bus interface
