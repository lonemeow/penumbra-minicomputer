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
*   **[ABI Specification](./system/abi.md)** — Data models, calling conventions, and ELF details.
*   **[Toolchain Strategy](./system/toolchain.md)** — LLVM backend and compiler considerations.

### System Services
*   **[MMU & TLB](./system/mmu.md)** — Software-managed translation, protection, and dirty tracking.
*   **[System Registers](./system/sysregs.md)** — Device-mapped control registers (Device Map).
*   **[Bus & Address Map](./system/bus.md)** — Physical memory layout and the autoconfig software flow.
*   **[Device Classes](./system/device-classes.md)** — The autoconfig class contract: what claiming a class *requires*.
*   **[Boot Protocol](./system/boot-protocol.md)** — The three-stage ROM → Loader → Kernel handoff.

### Device Drivers
*   **[UART](./system/devices/uart.md)** — Word-strided NS16550A console.
*   **[SPI / SD Card](./system/devices/spi.md)** — Register map and driver flow for storage.
*   **[ESP32 WiFi NIC](./system/devices/esp32-nic.md)** — Ethernet-over-SLIP network adapter interface.

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
*   **[WiFi NIC Hardware](./hardware/esp32-nic-hw.md)** — Physical pinouts and UART bridge design.

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
*   **[MMU Internals](./internals/mmu-internals.md)** — TLB hardware structure, discrete-logic mapping, and the VIPT L1 alias-free invariant.
*   **[MUL/DIV Unit (divmul)](./internals/divmul.md)** — The shared multiply/divide peer unit: algorithm, handshake, datapath, and discrete chip-count.
*   **[L2 Cache](./internals/l2-cache.md)** — Design plan and phase status for the optional unified L2 cache (currently write-invalidate-on-hit; write-back is a planned phase) between `cpu_core.o_mem_*` and the system bus.
*   **[SDRAM Controller v2](./internals/sdram-controller.md)** — Design plan for the rewrite (composable, dual-domain, 100 MHz CL2).
*   **[SDRAM Optimization](./internals/sdram-optimization.md)** — Controller strategies for bandwidth improvement.
*   **[Coding Standards](./internals/coding-standards.md)** — RTL naming and style conventions for hardware.
*   **[Development Setup](./internals/setup.md)** — How to build and simulate the RTL.

### Penumbra/1 (microcoded core)

*   **[Datapath Design](./internals/penumbra1/datapath.md)** — The three-bus architecture and signal flow.
*   **[Microcode Reference](./internals/penumbra1/microcode.md)** — Bit-level micro-word format and routine catalog.
*   **[Microcode Syntax](./internals/penumbra1/uasm-syntax.md)** — Writing microcode assembly for `uasm.py`.
*   **[L1 Cache](./internals/penumbra1/l1-cache.md)** — The gen1 distributed-RAM VIPT L1: geometry, write policy, and the defining zero-cycle hit.

### Penumbra/2 (gen2 — in planning)

The project's next major **CPU design era transition** — from
Penumbra/1's classic discrete-logic / microcoded minicomputer style
(~1970s-early-80s era) to the **simple pipelined RISC era of the late
1980s** (MIPS R2000/R3000, early SPARC). Same ISA as Penumbra/1, same
NetBSD kernel boots on both, almost all of the surrounding system (bus,
peripherals, MMU, L2, SDRAM) shared unchanged. The CPU core forks:
hardwired control, 6-stage pipeline, BRAM-backed caches. gen2 targets
correctness over performance; gen2.5 will add the early-1990s polish
(forwarding, branch prediction).

*   **[Penumbra/2 Overview](./internals/penumbra2/overview.md)** — Start here. Goals, architecture summary, gen2 / gen2.5 / future roadmap, reading guide.
*   **[Penumbra/2 Design Decisions](./internals/penumbra2/design-decisions.md)** — The 11 architectural decisions, each with full rationale, alternatives considered, and consequences.
*   **[Penumbra/2 Pipeline Stages](./internals/penumbra2/pipeline-stages.md)** — Per-stage description, inter-stage register layouts, stall/squash semantics, cycle-accurate timing examples.
*   **[Penumbra/2 Hazard Model](./internals/penumbra2/hazard-model.md)** — The scoreboard: storage, valid-bit lifecycle, stall predicate, ISA→physical register mapping, S/I control-state serialization, flag (NZCV) hazards, and divmul / drain-commit / exception-entry interactions.
*   **[Penumbra/2 Exception Flow](./internals/penumbra2/exception-flow.md)** — Taking and returning from exceptions/traps/interrupts in the pipeline: fault detection by stage, precise-exception commit at WB, save-state pulse, vector-fetch FSM, ERET, and interrupt recognition (SR.I, ei_shadow, EI/DI).
*   **[Penumbra/2 Control Decode](./internals/penumbra2/control-decode.md)** — Turning the instruction word into control: hardwired per-stage decode, the four instruction formats, the ID control bundle, register/immediate extraction, branch-condition evaluation, and the IR[15:12] aliasing hazard.
*   **[Penumbra/2 Register File](./internals/penumbra2/regfile.md)** — The 2R/1W register file: storage organisation, R0/R15 overrides, R14 (USP/SSP) banking and cross-bank access, and how divmul sequences its two-register result through the single write port.

---

## 🚀 [Roadmap](./TODO.md)
*   **[TODO & Roadmap](./TODO.md)** — Current tasks and long-term project goals.
