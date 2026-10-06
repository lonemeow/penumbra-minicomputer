# Penumbra Documentation

Welcome to the Penumbra Minicomputer documentation. This directory is
organized by audience and technical domain to ensure information is
relevant and easy to find.

## 📚 [System Reference](./system)
**Target:** Compiler writers, OS porters, and driver developers.

This section defines the programmer-visible behavior of the Penumbra
architecture and its standard peripherals.

### ISA & Software Environment
*   **[Architecture Overview](./system/architecture.md)** — Design philosophy, registers, and memory model.
*   **[Instruction Set Guide](./system/instruction-set.md)** — Programmer's reference for all instructions.
*   **[Instruction Encoding](./system/instruction-encoding.md)** — Bit-level reference for assembler / disassembler / binary-tool authors.
*   **[Instruction Timing](./system/instruction-timing.md)** — What each instruction costs on each generation, what memory typically costs, and what that means for code.
*   **[ABI Specification](./system/abi.md)** — Data models, calling conventions, and ELF details.
*   **[Toolchain Strategy](./system/toolchain.md)** — LLVM backend and compiler considerations.
*   **[LLVM Backend Notes](./llvm/README.md)** — How the backend's lowerings work, and upstream GlobalISel gap write-ups.

### System Services
*   **[MMU & TLB](./system/mmu.md)** — Software-managed translation, protection, and dirty tracking.
*   **[System Registers](./system/sysregs.md)** — Device-mapped control registers (Device Map).
*   **[Bus & Address Map](./system/bus.md)** — Physical memory layout and the autoconfig software flow.
*   **[Device Classes](./system/device-classes.md)** — The autoconfig class contract: what claiming a class *requires*.
*   **[Boot Protocol](./system/boot-protocol.md)** — The three-stage ROM → Loader → Kernel handoff.

### Device Drivers
*   **[UART](./system/devices/uart.md)** — Word-strided NS16550A console.
*   **[SPI / SD Card](./system/devices/spi.md)** — Register map and driver flow for storage.
*   **[USB host controller](./system/devices/usb-host.md)** — register interface and transaction contract; carries storage, HID, hubs, and the `ure(4)` Ethernet adapter that gives the machine its network.
*   **[Display Adapter](./system/devices/display.md)** — Character-cell console + optional framebuffer (`CLASS_DISPLAY`).
*   **[Audio Device](./system/devices/audio.md)** — Pushed-PCM output with optional mixing channels (`CLASS_AUDIO`).
*   **[USB Host Controller](./system/devices/usb-host.md)** — Transaction-level USB host (`CLASS_USBHC`).
*   **[Ethernet Adapter](./system/devices/ethernet.md)** — Whole frames through slot rings in device memory (`CLASS_ETHERNET`).
*   **[PS/2 Port](./system/devices/ps2.md)** — Bytes to and from one keyboard or pointing device (`CLASS_PS2`).

### Operating System
*   **[NetBSD Port Status](./system/netbsd/porting-status.md)** — Current status of the NetBSD 10 port.
*   **[NetBSD Design Notes](./system/netbsd/design-notes.md)** — Architectural decisions for the Penumbra port.

---

## 🛠️ [Hardware Design](./hardware)
**Target:** Board designers, external device implementers, and RTL engineers writing bus-protocol-compliant modules.

Signal-level specifications for the Penumbra Bus (both sync and async
forms) and related hardware interfaces.

*   **[Bus Protocol](./hardware/bus-protocol.md)** — Penumbra Bus terminology, async 4-phase handshake, sync-form mapping, signal timing.
*   **[Autoconfig Hardware](./hardware/autoconfig-hardware.md)** — Electrical implementation of the `cfg` daisy-chain.
*   **[SPI Hardware](./hardware/spi-hardware.md)** — Internal state machines and 74xx feasibility.

---

## 🔬 [Penumbra Internals](./internals)
**Target:** RTL/FPGA engineers working on the Penumbra core.

Detailed design of the CPU core and internal FPGA logic. These docs
are organized by **which design they describe**: *shared* internals
(both generations), *Penumbra/1* (microcoded core), and *Penumbra/2*
(pipelined core). Every internals doc carries an **Applies to:** banner
so its scope is clear at a glance. The governing rule: a root doc
states the architectural invariant or contract; each generation's
subdirectory owns its realization.

### Shared internals (all generations)

*   **[CPU-Internal Bus](./internals/cpu-bus.md)** — Contracts inside `cpu_core` (core ↔ MMU ↔ caches ↔ private sysreg devices) that both cores satisfy.
*   **[System Bus Implementation](./internals/system-bus.md)** — How the machine tops realise the sync-form Penumbra Bus: device decode, the response fabric, and the simple-device handshake cell.
*   **[MMU Internals](./internals/mmu-internals.md)** — TLB hardware structure, discrete-logic mapping, and the VIPT L1 alias-free invariant.
*   **[MUL/DIV Unit (divmul)](./internals/divmul.md)** — The shared multiply/divide peer unit: algorithm, handshake, datapath, and discrete chip-count.
*   **[Instruction Timing: Mechanism and Measurement](./internals/instruction-timing.md)** — Where each core's cycles come from, and how the figures in the programmer's timing reference are measured.
*   **[L2 Cache](./internals/l2-cache.md)** — Design plan and phase status for the optional unified L2 cache (currently write-invalidate-on-hit; write-back is a planned phase) between `cpu_core.o_mem_*` and the system bus.
*   **[SDRAM Controller v2](./internals/sdram-controller.md)** — Design plan for the rewrite (composable, dual-domain, 100 MHz CL2).
*   **[SDRAM Optimization](./internals/sdram-optimization.md)** — Controller strategies for bandwidth improvement.
*   **[Display Adapter](./internals/display.md)** — FPGA microarchitecture of the `CLASS_DISPLAY` adapter: pixel generator + TMDS output PHY.
*   **[USB Host Controller](./internals/usb-host-controller.md)** — FPGA microarchitecture of the `CLASS_USBHC` controller: SIE, transaction FSM, and the NetBSD HCD mapping.
*   **[Ethernet Adapter](./internals/ethernet.md)** — FPGA microarchitecture of the `CLASS_ETHERNET` adapter: the module split, the clock domains and what crosses between them, and the seam a board's PHY attaches at.
*   **[Coding Standards](./internals/coding-standards.md)** — RTL naming and style conventions for hardware.
*   **[Build & Test Structure](./internals/build-system.md)** — The BOARD×CORE build matrix, the machine integration layer, and the `isa/` conformance split for test programs.
*   **[Development Setup](./internals/setup.md)** — How to build and simulate the RTL.

### Boards

Where a board's realization differs in ways that matter, its own
directory holds it; the shared internals above state what every board
honors.

*   **[ULX3S](./internals/ulx3s/overview.md)** — The ECP5 reference board: tops, clocking and reset, memory, devices and connectors, and the routing-bound fabric's timing and placement.
*   **[Nexys Video](./internals/nexysvideo/overview.md)** — The Artix-7 board: tops, clocking and reset, LEDs, console and SD, I/O standards and constraints, the thermal guard.
*   **[Nexys Video DDR3 Memory](./internals/nexysvideo/ddr3.md)** — 512 MiB of DDR3 behind UberDDR3 and the line stack: the glue, the controller's configuration and license boundary, the crossing, calibration and reset.
*   **[Nexys Video Audio](./internals/nexysvideo/audio.md)** — The ADAU1761 codec behind the audio device: the two interfaces and who drives each clock, the frame on the wire, the setup sequence and its fault codes, the jacks.
*   **[Nexys Video Ethernet](./internals/nexysvideo/ethernet.md)** — The RTL8211E PHY behind the Ethernet adapter: the RGMII adapter and the speeds it carries, the clocks, the PHY's reset, the station address.

### Penumbra/1 (microcoded core)

*   **[Datapath Design](./internals/penumbra1/datapath.md)** — The three-bus architecture and signal flow.
*   **[Microcode Reference](./internals/penumbra1/microcode.md)** — Bit-level micro-word format and routine catalog.
*   **[Microcode Syntax](./internals/penumbra1/uasm-syntax.md)** — Writing microcode assembly for `uasm.py`.
*   **[L1 Cache](./internals/penumbra1/l1-cache.md)** — The gen1 distributed-RAM VIPT L1: geometry, write policy, and the defining zero-cycle hit.

### Penumbra/2 (gen2 / gen2.5 — abandoned; superseded by gen3)

> **⚠️ Abandoned.** gen2/2.5 was built and boots NetBSD, but tops out at
> 37.5 MHz on the ULX3S's ECP5 with no headroom for the remaining
> gen2.5 features — a
> fundamental in-order pure-stall floor. The docs below are retained as the
> **historical record**; gen3 is a fresh redo of the same in-order pipeline
> to clear that floor. See the abandonment note in
> [the gen2 overview](./internals/penumbra2/overview.md).

The project's first major **CPU design era transition** — from
Penumbra/1's classic discrete-logic / microcoded minicomputer style
(~1970s-early-80s era) to the **simple pipelined RISC era of the late
1980s** (MIPS R2000/R3000, early SPARC). Same ISA as Penumbra/1, same
NetBSD kernel boots on both, almost all of the surrounding system (bus,
peripherals, MMU, L2, SDRAM) shared unchanged. The CPU core forks:
hardwired control, 6-stage pipeline, BRAM-backed caches. gen2 targeted
correctness over performance; gen2.5 added the early-1990s polish
(forwarding, fetch-time branch prediction).

*   **[Penumbra/2 Overview](./internals/penumbra2/overview.md)** — Start here. Goals, architecture summary, gen2 / gen2.5 / future roadmap, reading guide.
*   **[Penumbra/2 Design Decisions](./internals/penumbra2/design-decisions.md)** — The 11 architectural decisions, each with full rationale, alternatives considered, and consequences.
*   **[Penumbra/2 Pipeline Stages](./internals/penumbra2/pipeline-stages.md)** — Per-stage description, inter-stage register layouts, stall/flush semantics, cycle-accurate timing examples.
*   **[Penumbra/2 Hazard Model](./internals/penumbra2/hazard-model.md)** — The scoreboard: storage, valid-bit lifecycle, stall predicate, ISA→physical register mapping, S/I control-state serialization, flag (NZCV) hazards, and divmul / drain-commit / exception-entry interactions.
*   **[Penumbra/2 Exception Flow](./internals/penumbra2/exception-flow.md)** — Taking and returning from exceptions/traps/interrupts in the pipeline: fault detection by stage, precise-exception commit at WB, save-state pulse, vector-fetch FSM, ERET, and interrupt recognition (SR.I, ei_shadow, EI/DI).
*   **[Penumbra/2 Control Decode](./internals/penumbra2/control-decode.md)** — Turning the instruction word into control: hardwired per-stage decode, the four instruction formats, the ID control bundle, register/immediate extraction, branch-condition evaluation, and the IR[15:12] aliasing hazard.
*   **[Penumbra/2 Register File](./internals/penumbra2/regfile.md)** — The 2R/1W register file: storage organisation, R0/R15 overrides, R14 (USP/SSP) banking and cross-bank access, and how divmul sequences its two-register result through the single write port.

### Penumbra/4 (gen4 — ideas and measurements)

An in-order core that issues two instructions a cycle: the generation
after gen3, on the same ISA. Nothing is built; these collect the ideas
and the measurements behind them.

*   **[Penumbra/4 Overview](./internals/penumbra4/overview.md)** — Start here. Why dual issue, the shape of the two pipelines, what carries over from gen3, and what is open.
*   **[Penumbra/4 Fusion and Pairing](./internals/penumbra4/issue-pairing.md)** — What each instruction fusion and each pairing rule is worth on real workloads, measured with the ISS's pairing model, and the issue stage the numbers point to.
*   **[Penumbra/4 Load-Use Wait](./internals/penumbra4/load-use.md)** — What the two-cycle wait behind a load costs on real workloads, and what a second ALU behind the memory stages would take back.
*   **[Penumbra/4 Register File](./internals/penumbra4/regfile.md)** — The structures that give a register file four read ports and two write ports, compared on the ECP5 and the Artix-7, and the one chosen.
*   **[Penumbra/4 Front End](./internals/penumbra4/front-end.md)** — How many instructions a cycle fetch has to supply, what the cycle after a taken transfer costs, and how far the fuser has to see.

---

## 📊 Performance
**Target:** Anyone asking how fast the machine is, and beside what.

*   **[Dhrystone Comparison](./dhrystone-comparison.md)** — DMIPS/MHz of each generation beside historical and soft cores of the same microarchitecture class, how the peer figures were chosen, and what the scores are made of.

---

## 🚀 [Roadmap](./TODO/README.md)
*   **[TODO & Roadmap](./TODO/README.md)** — Current tasks and long-term project goals.
